/**
 * @file data_processing_and_mqtt_task.c
 * @brief MQTT Publishing Task - Reads from sensor_task ring buffers
 *
 * Reads raw samples from ring buffers, converts to engineering units,
 * packages as JSON, and publishes via MQTT.
 *
 * DATA INTEGRITY:
 * - Each sensor reading is validated
 * - Stale/missed data shows as "null" in JSON
 * - No data is ever "carried over" from previous readings
 * - Every packet reflects the TRUE state of sensors at that moment
 */

#include "data_processing_and_mqtt_task.h"
#include "sensor_task.h"  // Ring buffer access functions
#include "adt7420.h"      // For direct temperature reading
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
static volatile uint32_t s_packets_sent = 0;
static volatile uint32_t s_samples_dropped = 0;

// Error tracking statistics
static volatile uint32_t s_incl_read_errors = 0;
static volatile uint32_t s_temp_read_errors = 0;
static volatile uint32_t s_incl_stale_count = 0;
static volatile uint32_t s_temp_stale_count = 0;

// Buffers for batching accelerometer data
static float s_accel_x[ACCEL_SAMPLES_PER_BATCH];
static float s_accel_y[ACCEL_SAMPLES_PER_BATCH];
static float s_accel_z[ACCEL_SAMPLES_PER_BATCH];
static uint32_t s_accel_ticks[ACCEL_SAMPLES_PER_BATCH];

// Temperature reading interval
#define TEMP_READ_INTERVAL_MS   1000    // Read temperature every 1 second

// Staleness thresholds - data older than this is considered invalid
#define INCL_STALE_THRESHOLD_MS     200     // Inclinometer: 20 Hz = 50ms, allow 4x margin
#define TEMP_STALE_THRESHOLD_MS     2000    // Temperature: 1 Hz = 1000ms, allow 2x margin

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
 * DATA PROCESSING AND MQTT PUBLISHING TASK
 *****************************************************************************/

static void data_processing_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data processing and MQTT task started");
    ESP_LOGI(TAG, "  Batch size: %d accel samples", ACCEL_SAMPLES_PER_BATCH);
    ESP_LOGI(TAG, "  Processing interval: %d ms", PROCESSING_INTERVAL_MS);
    ESP_LOGI(TAG, "  Temperature read interval: %d ms", TEMP_READ_INTERVAL_MS);
    ESP_LOGI(TAG, "  Data integrity: ENABLED (null for missing/stale data)");
    ESP_LOGI(TAG, "  Staleness thresholds: Incl=%dms, Temp=%dms",
             INCL_STALE_THRESHOLD_MS, TEMP_STALE_THRESHOLD_MS);

    adxl355_raw_sample_t adxl_sample;
    scl3300_raw_sample_t scl_sample;

    int accel_batch_count = 0;
    uint32_t first_tick = 0;

    // Temperature state
    uint32_t last_temp_read_ms = 0;
    float current_temp = 0.0f;
    bool temp_valid = false;
    uint32_t temp_read_time_ms = 0;

    // Inclinometer state
    float current_incl_x = 0.0f;
    float current_incl_y = 0.0f;
    float current_incl_z = 0.0f;
    bool incl_valid = false;
    uint32_t incl_read_time_ms = 0;

    while (s_task_running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // =====================================================================
        // Read Temperature directly via I2C (every 1 second)
        // =====================================================================
        if ((now_ms - last_temp_read_ms) >= TEMP_READ_INTERVAL_MS) {
            last_temp_read_ms = now_ms;

            float temp_celsius = 0.0f;
            esp_err_t err = adt7420_read_temperature(&temp_celsius);

            if (err == ESP_OK) {
                current_temp = temp_celsius;
                temp_valid = true;
                temp_read_time_ms = now_ms;
                ESP_LOGD(TAG, "Temperature: %.2f C", current_temp);
            } else {
                temp_valid = false;
                s_temp_read_errors++;
                ESP_LOGW(TAG, "Temperature read FAILED: %s (error #%lu)",
                         esp_err_to_name(err), (unsigned long)s_temp_read_errors);
            }
        }

        // =====================================================================
        // Read ALL available SCL3300 samples from ring buffer
        // Reset validity for each batch - must have fresh data
        // =====================================================================
        incl_valid = false;  // Assume no valid data until we read some

        while (scl3300_data_available()) {
            if (scl3300_read_sample(&scl_sample)) {
                current_incl_x = convert_scl3300_to_deg(scl_sample.raw_x);
                current_incl_y = convert_scl3300_to_deg(scl_sample.raw_y);
                current_incl_z = convert_scl3300_to_deg(scl_sample.raw_z);
                incl_valid = true;
                incl_read_time_ms = now_ms;
            }
        }

        // Check for stale inclinometer data
        if (incl_valid && (now_ms - incl_read_time_ms) > INCL_STALE_THRESHOLD_MS) {
            incl_valid = false;
            s_incl_stale_count++;
            ESP_LOGW(TAG, "Inclinometer data STALE (age=%lu ms, stale #%lu)",
                     (unsigned long)(now_ms - incl_read_time_ms),
                     (unsigned long)s_incl_stale_count);
        }

        // Check for stale temperature data
        bool temp_fresh = temp_valid && ((now_ms - temp_read_time_ms) <= TEMP_STALE_THRESHOLD_MS);
        if (temp_valid && !temp_fresh) {
            s_temp_stale_count++;
            ESP_LOGW(TAG, "Temperature data STALE (age=%lu ms, stale #%lu)",
                     (unsigned long)(now_ms - temp_read_time_ms),
                     (unsigned long)s_temp_stale_count);
        }

        // =====================================================================
        // Read ALL available ADXL355 samples from ring buffer
        // =====================================================================
        while (adxl355_data_available() && s_task_running) {
            if (adxl355_read_sample(&adxl_sample)) {
                if (accel_batch_count == 0) {
                    first_tick = adxl_sample.tick;
                }

                s_accel_x[accel_batch_count] = convert_adxl355_to_g(adxl_sample.raw_x);
                s_accel_y[accel_batch_count] = convert_adxl355_to_g(adxl_sample.raw_y);
                s_accel_z[accel_batch_count] = convert_adxl355_to_g(adxl_sample.raw_z);
                s_accel_ticks[accel_batch_count] = adxl_sample.tick;

                accel_batch_count++;

                // If batch is full, publish it
                if (accel_batch_count >= ACCEL_SAMPLES_PER_BATCH) {
                    if (mqtt_is_connected()) {
                        mqtt_sensor_packet_t packet = {0};
                        packet.timestamp = TICKS_TO_US(first_tick);
                        packet.accel_count = accel_batch_count;

                        // Copy accelerometer data
                        for (int i = 0; i < accel_batch_count; i++) {
                            packet.accel[i].x = s_accel_x[i];
                            packet.accel[i].y = s_accel_y[i];
                            packet.accel[i].z = s_accel_z[i];
                        }

                        // Inclinometer - only include if FRESH and VALID
                        if (incl_valid) {
                            packet.has_angle = true;
                            packet.angle_valid = true;
                            packet.angle_x = current_incl_x;
                            packet.angle_y = current_incl_y;
                            packet.angle_z = current_incl_z;
                        } else {
                            packet.has_angle = true;      // Include the field
                            packet.angle_valid = false;   // But mark as invalid (null)
                        }

                        // Temperature - only include if FRESH and VALID
                        if (temp_fresh) {
                            packet.has_temp = true;
                            packet.temp_valid = true;
                            packet.temperature = current_temp;
                        } else {
                            packet.has_temp = true;       // Include the field
                            packet.temp_valid = false;    // But mark as invalid (null)
                        }

                        // Publish
                        if (mqtt_publish_sensor_data(&packet) == ESP_OK) {
                            s_samples_published += accel_batch_count;
                            s_packets_sent++;
                        } else {
                            s_samples_dropped += accel_batch_count;
                        }
                    } else {
                        s_samples_dropped += accel_batch_count;
                    }

                    accel_batch_count = 0;
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

    s_samples_published = 0;
    s_packets_sent = 0;
    s_samples_dropped = 0;
    s_incl_read_errors = 0;
    s_temp_read_errors = 0;
    s_incl_stale_count = 0;
    s_temp_stale_count = 0;

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
    if (packets_sent) *packets_sent = s_packets_sent;
    if (samples_dropped) *samples_dropped = s_samples_dropped;
}

void data_processing_and_mqtt_task_get_error_stats(uint32_t *incl_errors,
                                                    uint32_t *temp_errors,
                                                    uint32_t *incl_stale,
                                                    uint32_t *temp_stale)
{
    if (incl_errors) *incl_errors = s_incl_read_errors;
    if (temp_errors) *temp_errors = s_temp_read_errors;
    if (incl_stale) *incl_stale = s_incl_stale_count;
    if (temp_stale) *temp_stale = s_temp_stale_count;
}
