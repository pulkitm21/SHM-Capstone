/**
 * @file data_processing_and_mqtt_task.c
 * @brief Data Processing Task - Reads sensor ring buffers, builds packets,
 *        dumps contents to console for debug verification.
 *
 * Sensor handling:
 *  - ADXL355 (1000 Hz raw): decimated 5:1 -> 200 Hz output, batched into
 *    groups of ACCEL_SAMPLES_PER_BATCH (10 samples = 50 ms per packet)
 *  - SCL3300 (20 Hz): read from ring buffer each cycle; values are STICKY -
 *    the last valid reading is kept until a new one arrives
 *  - ADT7420 (1 Hz): polled directly via I2C every 1 s; value is STICKY -
 *    the last valid reading is kept until a new one arrives
 *
 * PTP verification:
 *  - Each packet timestamp is derived from tick_counter (125 us per tick)
 *  - The inter-packet delta is logged alongside each packet
 *  - Expected delta: 50,000 us (50 ms) at steady state
 *  - Jitter or drift here indicates a PTP or acquisition timing problem
 */

#include "data_processing_and_mqtt_task.h"
#include "sensor_task.h"
#include "adt7420.h"
#include "mqtt.h"
#include "fault_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

/* sensor_task.c public API used here */

static const char *TAG = "DATA_PROC";

static TaskHandle_t s_task_handle = NULL;
static volatile bool s_task_running = false;

// Statistics
static volatile uint32_t s_samples_published = 0;
static volatile uint32_t s_packets_sent       = 0;
static volatile uint32_t s_samples_dropped    = 0;

// Error counters
static volatile uint32_t s_temp_read_errors = 0;

// Previous overflow counts for fault detection (compare each cycle to detect new drops)
static uint32_t s_prev_adxl355_overflow = 0;
static uint32_t s_prev_scl3300_overflow = 0;
static uint32_t s_prev_adt7420_overflow = 0;

// Accelerometer batch buffers
static float    s_accel_x[ACCEL_SAMPLES_PER_BATCH];
static float    s_accel_y[ACCEL_SAMPLES_PER_BATCH];
static float    s_accel_z[ACCEL_SAMPLES_PER_BATCH];
static uint32_t s_accel_ticks[ACCEL_SAMPLES_PER_BATCH];

// Temperature polling interval
#define TEMP_READ_INTERVAL_MS   1000

// Expected inter-packet interval in microseconds:
//   ACCEL_SAMPLES_PER_BATCH decimated samples * ACCEL_DECIM_FACTOR raw samples
//   each at 1 / ACCEL_RAW_RATE_HZ seconds = 10 * 5 * 1000 us = 50,000 us
#define EXPECTED_PACKET_INTERVAL_US \
    ((uint32_t)(ACCEL_SAMPLES_PER_BATCH) * (uint32_t)(ACCEL_DECIM_FACTOR) * \
     (1000000u / (uint32_t)(ACCEL_RAW_RATE_HZ)))

/******************************************************************************
 * UNIT CONVERSION FUNCTIONS
 *****************************************************************************/

static inline float convert_adxl355_to_g(int32_t raw)
{
    // ADXL355 ±2g range: 256000 LSB/g
    return (float)raw * (1.0f / 256000.0f);
}

static inline float convert_scl3300_to_deg(int16_t raw)
{
    // SCL3300 angle output registers: full-scale ±90° over 16384 LSB
    // Datasheet section 4.2: ANG_X/Y/Z = raw * (90 / 16384)
    return (float)raw * (90.0f / 16384.0f);
}

/*
 * Format a wall-clock timestamp into buf (must be >= MQTT_TS_LEN bytes).
 * tick is the ISR tick value captured at sample time (125 µs per tick).
 * If SNTP has not synced yet, writes "tick:NNNNNNNN" as a fallback so the
 * field is always populated and parseable by the receiver.
 */
static void format_ts(char *buf, uint32_t tick)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    if (tv.tv_sec > 1700000000L) {
        // Back-calculate the wall time when this tick was captured
        uint32_t current_tick = get_tick_count();
        int64_t delta_us = (int64_t)((int32_t)(current_tick - tick)) * 125LL;
        int64_t sample_us = (int64_t)tv.tv_sec * 1000000LL
                            + (int64_t)tv.tv_usec
                            - delta_us;

        time_t sec = (time_t)(sample_us / 1000000LL);
        int    us  = (int)(sample_us % 1000000LL);
        if (us < 0) { us += 1000000; sec -= 1; }

        struct tm tm_info;
        gmtime_r(&sec, &tm_info);
        strftime(buf, MQTT_TS_LEN, "%Y-%m-%dT%H:%M:%S", &tm_info);
        snprintf(buf + 19, MQTT_TS_LEN - 19, ".%06dZ", us);
    } else {
        snprintf(buf, MQTT_TS_LEN, "tick:%08lu", (unsigned long)tick);
    }
}

/******************************************************************************
 * DATA PROCESSING TASK
 *****************************************************************************/

static void data_processing_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data processing task started");
    ESP_LOGI(TAG, "  Batch size:          %d accel samples", ACCEL_SAMPLES_PER_BATCH);
    ESP_LOGI(TAG, "  Processing interval: %d ms",            PROCESSING_INTERVAL_MS);
    ESP_LOGI(TAG, "  Decimation:          %d:1 (%d Hz -> %d Hz)",
             ACCEL_DECIM_FACTOR, ACCEL_RAW_RATE_HZ, ACCEL_RAW_RATE_HZ / ACCEL_DECIM_FACTOR);
    ESP_LOGI(TAG, "  Expected pkt delta:  %lu us",           (unsigned long)EXPECTED_PACKET_INTERVAL_US);
    ESP_LOGI(TAG, "  Temp read interval:  %d ms",            TEMP_READ_INTERVAL_MS);

    adxl355_raw_sample_t adxl_sample;
    scl3300_raw_sample_t scl_sample;

    int      accel_batch_count = 0;

    // Decimation accumulators
    int     decim_count      = 0;
    int64_t sum_x            = 0;
    int64_t sum_y            = 0;
    int64_t sum_z            = 0;
    uint32_t decim_first_tick = 0;

    // -------------------------------------------------------------------------
    // Temperature state - STICKY
    //   temp_valid stays true once a good read is obtained; it is only cleared
    //   if a subsequent read actively fails.  This means every packet carries
    //   the most recent valid temperature even if the 1 s poll hasn't fired yet.
    // -------------------------------------------------------------------------
    // Initialise last_temp_read_ms far enough in the past that the first poll
    // fires on the very first task iteration rather than waiting a full second.
    uint32_t last_temp_read_ms = (uint32_t)(esp_timer_get_time() / 1000)
                                 - TEMP_READ_INTERVAL_MS;
    float current_temp  = 0.0f;
    bool  temp_valid    = false;
    char  current_temp_ts[MQTT_TS_LEN] = {0};  // wall-clock snapshot at poll time

    // -------------------------------------------------------------------------
    // Inclinometer state - STICKY
    //   incl_valid stays true once a valid sample is seen.  The value is only
    //   overwritten when a new sample actually arrives from the ring buffer.
    //   This prevents every packet from showing null simply because the 20 Hz
    //   SCL3300 hadn't fired yet in the current 50 ms processing window.
    // -------------------------------------------------------------------------
    float current_incl_x = 0.0f;
    float current_incl_y = 0.0f;
    float current_incl_z = 0.0f;
    bool  incl_valid     = false;

    // PTP delta tracking removed - timestamps still present in each packet

    while (s_task_running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // =====================================================================
        // FAULT CHECK: ring buffer overflows
        // Compare current overflow counters to previous — any increase means
        // samples were dropped since the last processing cycle.
        // =====================================================================
        uint32_t cur_adxl355_overflow = adxl355_get_overflow_count();
        uint32_t cur_scl3300_overflow = scl3300_get_overflow_count();
        uint32_t cur_adt7420_overflow = adt7420_get_overflow_count();

        if (cur_adxl355_overflow != s_prev_adxl355_overflow) {
            fault_log_record(FAULT_ADXL355_DROPPED);
            ESP_LOGW(TAG, "ADXL355 samples dropped (overflow count: %lu)",
                     (unsigned long)cur_adxl355_overflow);
            s_prev_adxl355_overflow = cur_adxl355_overflow;
        }
        if (cur_scl3300_overflow != s_prev_scl3300_overflow) {
            fault_log_record(FAULT_SCL3300_DROPPED);
            ESP_LOGW(TAG, "SCL3300 samples dropped (overflow count: %lu)",
                     (unsigned long)cur_scl3300_overflow);
            s_prev_scl3300_overflow = cur_scl3300_overflow;
        }
        if (cur_adt7420_overflow != s_prev_adt7420_overflow) {
            fault_log_record(FAULT_ADT7420_DROPPED);
            ESP_LOGW(TAG, "ADT7420 samples dropped (overflow count: %lu)",
                     (unsigned long)cur_adt7420_overflow);
            s_prev_adt7420_overflow = cur_adt7420_overflow;
        }

        // =====================================================================
        // Poll temperature via I2C every TEMP_READ_INTERVAL_MS
        // NOTE: ADT7420 is on I2C - it must NOT be accessed from the ISR.
        //       It is polled here in task context only.
        // =====================================================================
        if ((now_ms - last_temp_read_ms) >= TEMP_READ_INTERVAL_MS) {
            last_temp_read_ms = now_ms;

            float temp_celsius = 0.0f;
            esp_err_t err = adt7420_read_temperature(&temp_celsius);

            if (err == ESP_OK) {
                current_temp = temp_celsius;
                temp_valid   = true;   // sticky - stays true from here on
                format_ts(current_temp_ts, get_tick_count()); // snapshot wall time now
            } else {
                // Only invalidate on an active failure, not on "not polled yet"
                temp_valid = false;
                s_temp_read_errors++;
                fault_log_record(FAULT_I2C_ERROR);
                ESP_LOGW(TAG, "Temperature read FAILED: %s (error #%lu)",
                         esp_err_to_name(err), (unsigned long)s_temp_read_errors);
            }
        }

        // =====================================================================
        // Drain all available SCL3300 samples from the ring buffer.
        // We keep the most recent valid reading (sticky) - we do NOT reset
        // incl_valid at the start of each cycle.
        // =====================================================================
        bool new_incl_this_cycle = false;

        while (scl3300_data_available()) {
            if (scl3300_read_sample(&scl_sample)) {
                current_incl_x      = convert_scl3300_to_deg(scl_sample.raw_x);
                current_incl_y      = convert_scl3300_to_deg(scl_sample.raw_y);
                current_incl_z      = convert_scl3300_to_deg(scl_sample.raw_z);
                incl_valid          = true;   // sticky
                new_incl_this_cycle = true;
            }
        }

        if (!new_incl_this_cycle && !incl_valid) {
            // Warn only on the very first processing cycle where we still have
            // no inclinometer data.  After that, the sticky value takes over
            // and repeated warnings would just spam the console.
            static bool s_incl_warned = false;
            if (!s_incl_warned) {
                ESP_LOGW(TAG, "No inclinometer data received yet (will retry)");
                s_incl_warned = true;
            }
        }

        // =====================================================================
        // Drain all available ADXL355 samples, apply decimation, batch & dump
        // =====================================================================
        while (adxl355_data_available() && s_task_running) {
            if (adxl355_read_sample(&adxl_sample)) {

                // Capture tick of the first raw sample in each decimation window
                if (decim_count == 0) {
                    decim_first_tick = adxl_sample.tick;
                }

                sum_x += (int64_t)adxl_sample.raw_x;
                sum_y += (int64_t)adxl_sample.raw_y;
                sum_z += (int64_t)adxl_sample.raw_z;
                decim_count++;

                if (decim_count >= ACCEL_DECIM_FACTOR) {
                    int32_t avg_raw_x = (int32_t)(sum_x / ACCEL_DECIM_FACTOR);
                    int32_t avg_raw_y = (int32_t)(sum_y / ACCEL_DECIM_FACTOR);
                    int32_t avg_raw_z = (int32_t)(sum_z / ACCEL_DECIM_FACTOR);

                    s_accel_x[accel_batch_count] = convert_adxl355_to_g(avg_raw_x);
                    s_accel_y[accel_batch_count] = convert_adxl355_to_g(avg_raw_y);
                    s_accel_z[accel_batch_count] = convert_adxl355_to_g(avg_raw_z);
                    s_accel_ticks[accel_batch_count] = decim_first_tick;
                    accel_batch_count++;

                    // Reset decimation window
                    decim_count = 0;
                    sum_x = 0;
                    sum_y = 0;
                    sum_z = 0;

                    if (accel_batch_count >= ACCEL_SAMPLES_PER_BATCH) {
                        // ---------------------------------------------------------
                        // Flush SCL3300 ring buffer before building the packet.
                        //
                        // The outer loop drains SCL3300 once per task iteration,
                        // but the ADXL355 while-loop can build multiple packets
                        // without returning to the outer loop.  Without this flush,
                        // incl_valid stays false for the entire first batch (the
                        // ISR starts producing SCL3300 samples ~100 us after start,
                        // but the task hasn't looped back yet to pick them up).
                        // ---------------------------------------------------------
                        while (scl3300_data_available()) {
                            if (scl3300_read_sample(&scl_sample)) {
                                current_incl_x = convert_scl3300_to_deg(scl_sample.raw_x);
                                current_incl_y = convert_scl3300_to_deg(scl_sample.raw_y);
                                current_incl_z = convert_scl3300_to_deg(scl_sample.raw_z);
                                incl_valid     = true;
                                new_incl_this_cycle = true;
                            }
                        }

                        // ---------------------------------------------------------
                        // Build packet
                        // ---------------------------------------------------------
                        mqtt_sensor_packet_t packet = {0};
                        packet.accel_count = accel_batch_count;

                        for (int i = 0; i < accel_batch_count; i++) {
                            packet.accel[i].x = s_accel_x[i];
                            packet.accel[i].y = s_accel_y[i];
                            packet.accel[i].z = s_accel_z[i];
                            format_ts(packet.accel[i].ts, s_accel_ticks[i]);
                        }

                        packet.has_angle   = true;
                        packet.angle_valid = incl_valid;
                        if (incl_valid) {
                            packet.angle_x = current_incl_x;
                            packet.angle_y = current_incl_y;
                            packet.angle_z = current_incl_z;
                            format_ts(packet.angle_ts, scl_sample.tick);
                        }

                        packet.has_temp   = true;
                        packet.temp_valid = temp_valid;
                        if (temp_valid) {
                            packet.temperature = current_temp;
                            memcpy(packet.temp_ts, current_temp_ts, MQTT_TS_LEN);
                        }

                        // ---------------------------------------------------------
                        // Publish via MQTT
                        // ---------------------------------------------------------
                        esp_err_t pub_ret = mqtt_publish_sensor_data(&packet);

                        if (pub_ret == ESP_OK) {
                            s_samples_published += accel_batch_count;
                            // Compact one-line log per published packet
                            ESP_LOGD(TAG, "pkt#%lu accel=%d incl=%s temp=%s",
                                     (unsigned long)s_packets_sent,
                                     packet.accel_count,
                                     packet.angle_valid ? "ok" : "null",
                                     packet.temp_valid  ? "ok" : "null");
                        } else {
                            // MQTT not connected yet — count dropped packets and
                            // log a warning at most once per second to avoid spam.
                            s_samples_dropped += accel_batch_count;
                            static uint32_t s_last_drop_warn_ms = 0;
                            uint32_t now_warn_ms = (uint32_t)(esp_timer_get_time() / 1000);
                            if ((now_warn_ms - s_last_drop_warn_ms) >= 1000) {
                                s_last_drop_warn_ms = now_warn_ms;
                                ESP_LOGW(TAG, "MQTT not connected - packets dropping "
                                         "(dropped so far: %lu samples)",
                                         (unsigned long)s_samples_dropped);
                            }
                        }

                        s_packets_sent++;
                        accel_batch_count = 0;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PROCESSING_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Data processing task stopped");
    vTaskDelete(NULL);
}

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

esp_err_t data_processing_and_mqtt_task_init(void)
{
    ESP_LOGI(TAG, "Initializing data processing task...");

    s_samples_published = 0;
    s_packets_sent      = 0;
    s_samples_dropped   = 0;
    s_temp_read_errors  = 0;

    s_task_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        data_processing_task,
        "data_proc",
        DATA_PROCESSING_TASK_STACK_SIZE,
        NULL,
        DATA_PROCESSING_TASK_PRIORITY,
        &s_task_handle,
        DATA_PROCESSING_TASK_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Task started (priority=%d, core=%d)",
             DATA_PROCESSING_TASK_PRIORITY, DATA_PROCESSING_TASK_CORE);

    return ESP_OK;
}

esp_err_t data_processing_and_mqtt_task_stop(void)
{
    ESP_LOGI(TAG, "Stopping data processing task...");
    s_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    s_task_handle = NULL;
    ESP_LOGI(TAG, "Task stopped");
    return ESP_OK;
}

void data_processing_and_mqtt_task_get_stats(uint32_t *samples_published,
                                              uint32_t *packets_sent,
                                              uint32_t *samples_dropped)
{
    if (samples_published) *samples_published = s_samples_published;
    if (packets_sent)      *packets_sent      = s_packets_sent;
    if (samples_dropped)   *samples_dropped   = s_samples_dropped;
}