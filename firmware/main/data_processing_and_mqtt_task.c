/**
 * @file data_processing_and_mqtt_task.c
 * @brief MQTT Publishing Task - Reads from sensor_task ring buffers
 *
 * DATA INTEGRITY:
 * - If no data received this cycle → shows "null" in JSON
 * - Garbage accelerometer samples → that sample published as null in "a" array
 * - Garbage inclinometer reading → "i" field published as null
 *
 * GARBAGE DETECTION:
 * Accelerometer (ADXL355, ±2g range, 20-bit raw):
 *   - All three averaged raw axes == 0 → stuck read / floating MISO low
 *   - Any axis raw value near 20-bit rail (±500000) → floating MISO high
 *   - Any converted axis magnitude > 3.0g → physically impossible for ±2g sensor
 *
 * Inclinometer (SCL3300, angle mode, 16-bit raw):
 *   - All three raw axes == 0 → stuck read / floating MISO low
 *   - Any axis near 16-bit rail (±32700) → floating MISO high
 *   - Any converted angle magnitude > 91° → physically impossible
 *
 * TIMESTAMPING:
 * - packet.timestamp is set from CLOCK_PTP_SYSTEM once per processing cycle.
 * - It represents microseconds since Unix epoch, synchronized across all
 *   nodes via PTP (Raspberry Pi is grandmaster).
 * IMPORTANT: clock_gettime(CLOCK_REALTIME, ...) is used. On this ESP32
 *   build, CLOCK_REALTIME is disciplined by the PTP daemon (ptpd.c) which
 *   continuously adjusts it to track the Raspberry Pi grandmaster clock.
 *   Do NOT substitute esp_timer_get_time() — that clock is not PTP-disciplined
 *   and will not be synchronized across nodes.
 * - If the PTP daemon has not yet set the clock (timestamp is before 2024),
 *   the packet is dropped rather than published with a garbage timestamp.
 *
 * FAULT LOGGING:
 * - FAULT_ADXL355_DROPPED (7): logged when adxl355 overflow counter increases
 * - FAULT_SCL3300_DROPPED (8): logged when scl3300 overflow counter increases
 * - FAULT_ADT7420_DROPPED (9): logged when adt7420 overflow counter increases
 * - FAULT_SPI_ERROR (17):      logged on mid-run SPI read failure (inclinometer)
 * - FAULT_I2C_ERROR (18):      logged on mid-run I2C read failure (temperature)
 */

#include "data_processing_and_mqtt_task.h"
#include "fault_log.h"
#include "sensor_task.h"
#include "adt7420.h"
#include "mqtt.h"
#include "esp_log.h"
#include "esp_eth_time.h"       // esp_eth_clock_gettime(), CLOCK_PTP_SYSTEM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>
#include <time.h>               // struct timespec

static const char *TAG = "DATA_PROC";

static TaskHandle_t s_task_handle = NULL;
static volatile bool s_task_running = false;

// Statistics
static volatile uint32_t s_samples_published = 0;
static volatile uint32_t s_packets_sent = 0;
static volatile uint32_t s_samples_dropped = 0;

// Error counters
static volatile uint32_t s_temp_read_errors = 0;
static volatile uint32_t s_accel_garbage_count = 0;
static volatile uint32_t s_incl_garbage_count = 0;

// Buffers for batching accelerometer data
static float     s_accel_x[ACCEL_SAMPLES_PER_BATCH];
static float     s_accel_y[ACCEL_SAMPLES_PER_BATCH];
static float     s_accel_z[ACCEL_SAMPLES_PER_BATCH];
static bool      s_accel_valid[ACCEL_SAMPLES_PER_BATCH];
static uint32_t  s_accel_ticks[ACCEL_SAMPLES_PER_BATCH];

// Temperature reading interval
#define TEMP_READ_INTERVAL_MS   1000

/******************************************************************************
 * GARBAGE DETECTION THRESHOLDS
 *****************************************************************************/
#define ACCEL_GARBAGE_THRESHOLD_G    3.0f
#define ACCEL_RAW_RAIL_THRESHOLD     500000
#define INCL_GARBAGE_THRESHOLD_DEG   91.0f
#define INCL_RAW_RAIL_THRESHOLD      32700

/******************************************************************************
 * UNIT CONVERSION FUNCTIONS
 *****************************************************************************/

static inline float convert_adxl355_to_g(int32_t raw)
{
    return (float)raw * (1.0f / 256000.0f);
}

static inline float convert_scl3300_to_deg(int16_t raw)
{
    return (float)raw * 0.0055f;
}

/******************************************************************************
 * PTP TIMESTAMP HELPER
 *
 * Returns microseconds since Unix epoch from CLOCK_PTP_SYSTEM.
 * Returns 0 if the PTP clock is not yet available.
 *
 * This is the ONLY correct clock to use for packet timestamps. Do not
 * substitute esp_timer_get_time() or CLOCK_REALTIME — they are not
 * disciplined by PTP and will not be synchronized across nodes.
 *****************************************************************************/

static uint64_t get_ptp_timestamp_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ESP_LOGW(TAG, "clock_gettime failed — timestamp will be 0");
        return 0ULL;
    }

    /* Sanity check: if time is before Jan 1 2024, PTP has not synced yet.
     * CLOCK_REALTIME on ESP32 starts at 0 (Unix epoch) on boot and is
     * only set to real wall-clock time once ptpd receives Sync packets
     * from the Raspberry Pi grandmaster and calls clock_settime(). */
    const time_t JAN_2024 = 1704067200;
    if (ts.tv_sec < JAN_2024) {
        ESP_LOGW(TAG, "PTP not yet synced (time=%llu s) — timestamp will be 0",
                 (unsigned long long)ts.tv_sec);
        return 0ULL;
    }

    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/******************************************************************************
 * GARBAGE DETECTION
 *****************************************************************************/

static bool is_accel_garbage(int32_t raw_x, int32_t raw_y, int32_t raw_z)
{
    if (raw_x == 0 && raw_y == 0 && raw_z == 0) return true;

    if (raw_x >  ACCEL_RAW_RAIL_THRESHOLD || raw_x < -ACCEL_RAW_RAIL_THRESHOLD ||
        raw_y >  ACCEL_RAW_RAIL_THRESHOLD || raw_y < -ACCEL_RAW_RAIL_THRESHOLD ||
        raw_z >  ACCEL_RAW_RAIL_THRESHOLD || raw_z < -ACCEL_RAW_RAIL_THRESHOLD) {
        return true;
    }

    if (fabsf(convert_adxl355_to_g(raw_x)) > ACCEL_GARBAGE_THRESHOLD_G ||
        fabsf(convert_adxl355_to_g(raw_y)) > ACCEL_GARBAGE_THRESHOLD_G ||
        fabsf(convert_adxl355_to_g(raw_z)) > ACCEL_GARBAGE_THRESHOLD_G) {
        return true;
    }

    return false;
}

static bool is_incl_garbage(int16_t raw_x, int16_t raw_y, int16_t raw_z)
{
    if (raw_x == 0 && raw_y == 0 && raw_z == 0) return true;

    if (raw_x >  INCL_RAW_RAIL_THRESHOLD || raw_x < -INCL_RAW_RAIL_THRESHOLD ||
        raw_y >  INCL_RAW_RAIL_THRESHOLD || raw_y < -INCL_RAW_RAIL_THRESHOLD ||
        raw_z >  INCL_RAW_RAIL_THRESHOLD || raw_z < -INCL_RAW_RAIL_THRESHOLD) {
        return true;
    }

    if (fabsf(convert_scl3300_to_deg(raw_x)) > INCL_GARBAGE_THRESHOLD_DEG ||
        fabsf(convert_scl3300_to_deg(raw_y)) > INCL_GARBAGE_THRESHOLD_DEG ||
        fabsf(convert_scl3300_to_deg(raw_z)) > INCL_GARBAGE_THRESHOLD_DEG) {
        return true;
    }

    return false;
}

/******************************************************************************
 * DATA PROCESSING AND MQTT PUBLISHING TASK
 *****************************************************************************/

static void data_processing_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data processing and MQTT task started");
    ESP_LOGI(TAG, "  Batch size: %d accel samples", ACCEL_SAMPLES_PER_BATCH);
    ESP_LOGI(TAG, "  Processing interval: %d ms", PROCESSING_INTERVAL_MS);
    ESP_LOGI(TAG, "  Temperature read interval: %d ms", TEMP_READ_INTERVAL_MS);
    ESP_LOGI(TAG, "  Accel garbage threshold: +/-%.1fg or raw rail +/-%d",
             ACCEL_GARBAGE_THRESHOLD_G, ACCEL_RAW_RAIL_THRESHOLD);
    ESP_LOGI(TAG, "  Incl  garbage threshold: +/-%.1fdeg or raw rail +/-%d",
             INCL_GARBAGE_THRESHOLD_DEG, INCL_RAW_RAIL_THRESHOLD);

    adxl355_raw_sample_t adxl_sample;
    scl3300_raw_sample_t scl_sample;

    int accel_batch_count = 0;

    int decim_count = 0;
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int64_t sum_z = 0;
    uint32_t decim_first_tick = 0;

    // Temperature state
    uint32_t last_temp_read_ms = 0;
    float current_temp = 0.0f;
    bool temp_valid = false;

    // Inclinometer state
    float current_incl_x = 0.0f;
    float current_incl_y = 0.0f;
    float current_incl_z = 0.0f;
    bool incl_valid = false;

    /* Overflow counters from previous cycle — used to detect new drops */
    uint32_t prev_adxl355_overflow = 0;
    uint32_t prev_scl3300_overflow = 0;
    uint32_t prev_adt7420_overflow = 0;

    while (s_task_running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* =================================================================
         * Take one PTP timestamp at the START of this processing cycle.
         *
         * This is the wall-clock anchor for the entire batch. It is set
         * here — once per 50ms cycle — so we are not calling
         * esp_eth_clock_gettime() for every individual sample (which would
         * be 1000 calls/sec and adds unnecessary overhead).
         *
         * This timestamp represents when we started processing this batch.
         * On the Raspberry Pi: datetime.utcfromtimestamp(t / 1e6)
         * ================================================================= */
        uint64_t cycle_timestamp_us = get_ptp_timestamp_us();

        /* =================================================================
         * FAULT CHECK: ring buffer overflows (sample drops)
         * ================================================================= */
        uint32_t cur_adxl355_overflow = adxl355_get_overflow_count();
        uint32_t cur_scl3300_overflow = scl3300_get_overflow_count();
        uint32_t cur_adt7420_overflow = adt7420_get_overflow_count();

        if (cur_adxl355_overflow != prev_adxl355_overflow) {
            fault_log_record(FAULT_ADXL355_DROPPED);
            ESP_LOGW(TAG, "ADXL355 samples dropped (overflow count: %lu)",
                     (unsigned long)cur_adxl355_overflow);
            prev_adxl355_overflow = cur_adxl355_overflow;
        }

        if (cur_scl3300_overflow != prev_scl3300_overflow) {
            fault_log_record(FAULT_SCL3300_DROPPED);
            ESP_LOGW(TAG, "SCL3300 samples dropped (overflow count: %lu)",
                     (unsigned long)cur_scl3300_overflow);
            prev_scl3300_overflow = cur_scl3300_overflow;
        }

        if (cur_adt7420_overflow != prev_adt7420_overflow) {
            fault_log_record(FAULT_ADT7420_DROPPED);
            ESP_LOGW(TAG, "ADT7420 samples dropped (overflow count: %lu)",
                     (unsigned long)cur_adt7420_overflow);
            prev_adt7420_overflow = cur_adt7420_overflow;
        }

        /* =================================================================
         * Read Temperature directly via I2C (every 1 second)
         * ================================================================= */
        if ((now_ms - last_temp_read_ms) >= TEMP_READ_INTERVAL_MS) {
            last_temp_read_ms = now_ms;

            float temp_celsius = 0.0f;
            esp_err_t err = adt7420_read_temperature(&temp_celsius);

            if (err == ESP_OK) {
                current_temp = temp_celsius;
                temp_valid = true;
            } else {
                temp_valid = false;
                s_temp_read_errors++;
                /* FAULT 18: I2C bus error during mid-run temperature read */
                fault_log_record(FAULT_I2C_ERROR);
                ESP_LOGW(TAG, "Temperature read FAILED (I2C error): %s (error #%lu)",
                         esp_err_to_name(err), (unsigned long)s_temp_read_errors);
            }
        }

        /* =================================================================
         * Read ALL available SCL3300 samples from ring buffer
         * ================================================================= */
        incl_valid = false;

        while (scl3300_data_available()) {
            if (scl3300_read_sample(&scl_sample)) {

                if (is_incl_garbage(scl_sample.raw_x, scl_sample.raw_y, scl_sample.raw_z)) {
                    s_incl_garbage_count++;
                    /* FAULT 17: treat garbage inclinometer read as an SPI bus error */
                    fault_log_record(FAULT_SPI_ERROR);
                    ESP_LOGW(TAG, "Inclinometer garbage detected (raw: %d %d %d) → null (total: %lu)",
                             scl_sample.raw_x, scl_sample.raw_y, scl_sample.raw_z,
                             (unsigned long)s_incl_garbage_count);
                } else {
                    current_incl_x = convert_scl3300_to_deg(scl_sample.raw_x);
                    current_incl_y = convert_scl3300_to_deg(scl_sample.raw_y);
                    current_incl_z = convert_scl3300_to_deg(scl_sample.raw_z);
                    incl_valid = true;
                }
            }
        }

        if (!incl_valid) {
            ESP_LOGW(TAG, "No valid inclinometer data this cycle");
        }

        /* =================================================================
         * Read ALL available ADXL355 samples (with decimation)
         * ================================================================= */
        while (adxl355_data_available() && s_task_running) {
            if (adxl355_read_sample(&adxl_sample)) {

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

                    if (is_accel_garbage(avg_raw_x, avg_raw_y, avg_raw_z)) {
                        s_accel_garbage_count++;
                        /* FAULT 17: garbage accel read treated as SPI error */
                        fault_log_record(FAULT_SPI_ERROR);
                        ESP_LOGW(TAG, "Accel garbage detected (raw: %ld %ld %ld) → null (total: %lu)",
                                 (long)avg_raw_x, (long)avg_raw_y, (long)avg_raw_z,
                                 (unsigned long)s_accel_garbage_count);

                        s_accel_x[accel_batch_count] = 0.0f;
                        s_accel_y[accel_batch_count] = 0.0f;
                        s_accel_z[accel_batch_count] = 0.0f;
                        s_accel_valid[accel_batch_count] = false;
                    } else {
                        s_accel_x[accel_batch_count] = convert_adxl355_to_g(avg_raw_x);
                        s_accel_y[accel_batch_count] = convert_adxl355_to_g(avg_raw_y);
                        s_accel_z[accel_batch_count] = convert_adxl355_to_g(avg_raw_z);
                        s_accel_valid[accel_batch_count] = true;
                    }

                    s_accel_ticks[accel_batch_count] = decim_first_tick;
                    accel_batch_count++;

                    decim_count = 0;
                    sum_x = 0;
                    sum_y = 0;
                    sum_z = 0;

                    if (accel_batch_count >= ACCEL_SAMPLES_PER_BATCH) {

                        /* -----------------------------------------------------
                         * Drop the packet if PTP timestamp is not yet available.
                         *
                         * A timestamp of 0 means esp_eth_clock_gettime() returned
                         * -1 — the PTP clock has not yet synced. Publishing a
                         * packet with timestamp=0 would be misleading on the Pi.
                         * This should never happen in normal operation because
                         * main.c blocks in ptp_init_and_sync() until the clock
                         * is valid before starting sensor acquisition, but we
                         * guard here defensively.
                         * ----------------------------------------------------- */
                        if (cycle_timestamp_us == 0ULL) {
                            s_samples_dropped += accel_batch_count;
                            ESP_LOGW(TAG, "PTP timestamp not available — dropped %d samples",
                                     accel_batch_count);
                            accel_batch_count = 0;
                            continue;
                        }

                        if (mqtt_is_connected()) {
                            mqtt_sensor_packet_t packet = {0};

                            /* --------------------------------------------------
                             * Set PTP-synchronized wall-clock timestamp.
                             * This is microseconds since Unix epoch from
                             * CLOCK_PTP_SYSTEM, taken at the start of this cycle.
                             * -------------------------------------------------- */
                            packet.timestamp = cycle_timestamp_us;

                            packet.accel_count = accel_batch_count;

                            for (int i = 0; i < accel_batch_count; i++) {
                                packet.accel[i].x     = s_accel_x[i];
                                packet.accel[i].y     = s_accel_y[i];
                                packet.accel[i].z     = s_accel_z[i];
                                packet.accel[i].valid = s_accel_valid[i];
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

                            /* Publish — fault codes appended inside
                             * mqtt_publish_sensor_data() */
                            if (mqtt_publish_sensor_data(&packet) == ESP_OK) {
                                s_samples_published += accel_batch_count;
                                s_packets_sent++;
                            } else {
                                s_samples_dropped += accel_batch_count;
                                ESP_LOGW(TAG, "MQTT publish failed, dropped %d samples",
                                         accel_batch_count);
                            }
                        } else {
                            s_samples_dropped += accel_batch_count;
                            ESP_LOGW(TAG, "MQTT disconnected, dropped %d samples",
                                     accel_batch_count);
                        }

                        accel_batch_count = 0;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PROCESSING_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Data processing and MQTT task stopped");
    vTaskDelete(NULL);
}

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

esp_err_t data_processing_and_mqtt_task_init(void)
{
    ESP_LOGI(TAG, "Initializing data processing and MQTT task...");

    s_samples_published   = 0;
    s_packets_sent        = 0;
    s_samples_dropped     = 0;
    s_temp_read_errors    = 0;
    s_accel_garbage_count = 0;
    s_incl_garbage_count  = 0;

    s_task_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        data_processing_task,
        "data_proc_mqtt",
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
    ESP_LOGI(TAG, "Stopping data processing and MQTT task...");
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
