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
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "DATA_PROC";

static TaskHandle_t s_task_handle = NULL;
static volatile bool s_task_running = false;

// Statistics
static volatile uint32_t s_samples_published = 0;
static volatile uint32_t s_packets_sent       = 0;
static volatile uint32_t s_samples_dropped    = 0;

// Error counters
static volatile uint32_t s_temp_read_errors = 0;

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
    // SCL3300 angle output: 0.0055 deg/LSB
    return (float)raw * 0.0055f;
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
    uint32_t first_tick        = 0;

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

    // PTP delta tracking
    uint32_t last_packet_ts_us = 0;
    bool     first_packet      = true;

    while (s_task_running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

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
            } else {
                // Only invalidate on an active failure, not on "not polled yet"
                temp_valid = false;
                s_temp_read_errors++;
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
            // Only warn while we have never received any inclinometer data yet
            ESP_LOGW(TAG, "No inclinometer data received yet");
        }

        // =====================================================================
        // Drain all available ADXL355 samples, apply decimation, batch & dump
        // =====================================================================
        while (adxl355_data_available() && s_task_running) {
            if (adxl355_read_sample(&adxl_sample)) {

                // Capture tick of the very first raw sample in this packet
                if (accel_batch_count == 0 && decim_count == 0) {
                    first_tick = adxl_sample.tick;
                }

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
                        // Build packet
                        // ---------------------------------------------------------
                        mqtt_sensor_packet_t packet = {0};
                        packet.timestamp   = TICKS_TO_US(first_tick);
                        packet.accel_count = accel_batch_count;

                        for (int i = 0; i < accel_batch_count; i++) {
                            packet.accel[i].x = s_accel_x[i];
                            packet.accel[i].y = s_accel_y[i];
                            packet.accel[i].z = s_accel_z[i];
                        }

                        packet.has_angle   = true;
                        packet.angle_valid = incl_valid;
                        if (incl_valid) {
                            packet.angle_x = current_incl_x;
                            packet.angle_y = current_incl_y;
                            packet.angle_z = current_incl_z;
                        }

                        packet.has_temp   = true;
                        packet.temp_valid = temp_valid;
                        if (temp_valid) {
                            packet.temperature = current_temp;
                        }

                        // ---------------------------------------------------------
                        // PTP / timing delta
                        // ---------------------------------------------------------
                        uint32_t delta_us = 0;
                        if (!first_packet) {
                            delta_us = packet.timestamp - last_packet_ts_us;
                        }
                        last_packet_ts_us = packet.timestamp;
                        first_packet      = false;

                        // ---------------------------------------------------------
                        // Console dump
                        // ---------------------------------------------------------
                        ESP_LOGI(TAG, "===== PACKET #%lu =====",
                                 (unsigned long)s_packets_sent);

                        // Timestamp and inter-packet delta
                        if (s_packets_sent == 0) {
                            ESP_LOGI(TAG, "  ts = %lu us  (first packet)",
                                     (unsigned long)packet.timestamp);
                        } else {
                            int32_t jitter = (int32_t)delta_us
                                             - (int32_t)EXPECTED_PACKET_INTERVAL_US;
                            ESP_LOGI(TAG, "  ts = %lu us  |  delta = %lu us  "
                                         "(expected %lu us, jitter %+ld us)",
                                     (unsigned long)packet.timestamp,
                                     (unsigned long)delta_us,
                                     (unsigned long)EXPECTED_PACKET_INTERVAL_US,
                                     (long)jitter);
                        }

                        // Accelerometer
                        ESP_LOGI(TAG, "  Accel (%d samples @ %d Hz):",
                                 packet.accel_count,
                                 ACCEL_RAW_RATE_HZ / ACCEL_DECIM_FACTOR);
                        for (int i = 0; i < packet.accel_count; i++) {
                            ESP_LOGI(TAG, "    [%2d]  X=%8.5f g  Y=%8.5f g  Z=%8.5f g",
                                     i,
                                     (double)packet.accel[i].x,
                                     (double)packet.accel[i].y,
                                     (double)packet.accel[i].z);
                        }

                        // Inclinometer
                        if (!packet.angle_valid) {
                            ESP_LOGW(TAG, "  Incl:  null (no valid sample yet)");
                        } else {
                            ESP_LOGI(TAG, "  Incl:  X=%7.4f deg  Y=%7.4f deg  Z=%7.4f deg%s",
                                     (double)packet.angle_x,
                                     (double)packet.angle_y,
                                     (double)packet.angle_z,
                                     new_incl_this_cycle ? "  [fresh]" : "  [held]");
                        }

                        // Temperature
                        if (!packet.temp_valid) {
                            ESP_LOGW(TAG, "  Temp:  null (no valid read yet)");
                        } else {
                            ESP_LOGI(TAG, "  Temp:  %.3f C", (double)packet.temperature);
                        }

                        ESP_LOGI(TAG, "==========================================");

                        s_samples_published += accel_batch_count;
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
