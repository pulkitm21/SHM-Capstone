/**
 * @file data_processing_and_mqtt_task.c
 * @brief MQTT Publishing Task - Reads from sensor_task ring buffers
 *
 * Reads raw samples from ring buffers, converts to engineering units,
 * packages as JSON, and publishes via MQTT.
 *
 * NOTE: Temperature is read DIRECTLY in this task (not via ISR) because:
 * - I2C is slower than SPI
 * - Temperature only needs 1 Hz update rate
 * - Keeps ISR fast and deterministic
 */

#include "data_processing_and_mqtt_task.h"
#include "sensor_task.h"  // Ring buffer access functions
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
static volatile uint32_t s_packets_sent = 0;
static volatile uint32_t s_samples_dropped = 0;

// Buffers for batching accelerometer data
static float s_accel_x[ACCEL_SAMPLES_PER_BATCH];
static float s_accel_y[ACCEL_SAMPLES_PER_BATCH];
static float s_accel_z[ACCEL_SAMPLES_PER_BATCH];
static uint32_t s_accel_ticks[ACCEL_SAMPLES_PER_BATCH];

// Latest inclinometer and temperature values
static float s_latest_incl_x = 0.0f;
static float s_latest_incl_y = 0.0f;
static float s_latest_incl_z = 0.0f;
static bool s_has_incl_data = false;

static float s_latest_temp = 0.0f;
static bool s_has_temp_data = false;

// Temperature reading interval
#define TEMP_READ_INTERVAL_MS   1000    // Read temperature every 1 second

/******************************************************************************
 * UNIT CONVERSION FUNCTIONS
 *
 * All conversions happen HERE - sensor drivers only provide raw data.
 *****************************************************************************/

/**
 * @brief Convert raw ADXL355 value to g
 *
 * ADXL355 in ±2g range: 256,000 LSB/g
 * Scale factor = 1/256000 = 3.906e-6 g/LSB
 */
static inline float convert_adxl355_to_g(int32_t raw)
{
    return (float)raw * (1.0f / 256000.0f);
}

/**
 * @brief Convert raw SCL3300 angle value to degrees
 *
 * SCL3300 Mode 3 (Inclination): 0.0055 deg/LSB (from datasheet)
 */
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
    ESP_LOGI(TAG, "  Conversions:");
    ESP_LOGI(TAG, "    ADXL355: raw * (1/256000) = g");
    ESP_LOGI(TAG, "    SCL3300: raw * 0.0055 = degrees");
    ESP_LOGI(TAG, "    ADT7420: read directly via I2C");

    adxl355_raw_sample_t adxl_sample;
    scl3300_raw_sample_t scl_sample;

    int accel_batch_count = 0;
    uint32_t first_tick = 0;

    // Temperature reading timer
    uint32_t last_temp_read_ms = 0;

    while (s_task_running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // =====================================================================
        // Read Temperature directly via I2C (every 1 second)
        // =====================================================================
        if ((now_ms - last_temp_read_ms) >= TEMP_READ_INTERVAL_MS) {
            last_temp_read_ms = now_ms;

            // Use existing adt7420 driver function to read temperature
            float temp_celsius = 0.0f;
            esp_err_t err = adt7420_read_temperature(&temp_celsius);

            if (err == ESP_OK) {
                s_latest_temp = temp_celsius;
                s_has_temp_data = true;
                ESP_LOGD(TAG, "Temperature: %.2f C", s_latest_temp);
            } else {
                ESP_LOGW(TAG, "Failed to read temperature: %s", esp_err_to_name(err));
                // Keep old value, don't clear s_has_temp_data
            }
        }

        // =====================================================================
        // Read ALL available ADXL355 samples from ring buffer
        // =====================================================================
        while (adxl355_data_available() && s_task_running) {
            if (adxl355_read_sample(&adxl_sample)) {
                // Store first tick for timestamp
                if (accel_batch_count == 0) {
                    first_tick = adxl_sample.tick;
                }

                // Convert and store in batch buffer
                s_accel_x[accel_batch_count] = convert_adxl355_to_g(adxl_sample.raw_x);
                s_accel_y[accel_batch_count] = convert_adxl355_to_g(adxl_sample.raw_y);
                s_accel_z[accel_batch_count] = convert_adxl355_to_g(adxl_sample.raw_z);
                s_accel_ticks[accel_batch_count] = adxl_sample.tick;

                accel_batch_count++;

                // If batch is full, publish it
                if (accel_batch_count >= ACCEL_SAMPLES_PER_BATCH) {
                    if (mqtt_is_connected()) {
                        // Build and publish packet
                        mqtt_sensor_packet_t packet = {0};
                        packet.timestamp = TICKS_TO_US(first_tick);
                        packet.accel_count = accel_batch_count;

                        // Copy accelerometer data
                        for (int i = 0; i < accel_batch_count; i++) {
                            packet.accel[i].x = s_accel_x[i];
                            packet.accel[i].y = s_accel_y[i];
                            packet.accel[i].z = s_accel_z[i];
                        }

                        // Add latest inclinometer data if available
                        packet.has_angle = s_has_incl_data;
                        if (s_has_incl_data) {
                            packet.angle_x = s_latest_incl_x;
                            packet.angle_y = s_latest_incl_y;
                            packet.angle_z = s_latest_incl_z;
                        }

                        // Add latest temperature if available
                        packet.has_temp = s_has_temp_data;
                        if (s_has_temp_data) {
                            packet.temperature = s_latest_temp;
                        }

                        // Publish
                        if (mqtt_publish_sensor_data(&packet) == ESP_OK) {
                            s_samples_published += accel_batch_count;
                            s_packets_sent++;
                        } else {
                            s_samples_dropped += accel_batch_count;
                        }
                    } else {
                        // MQTT not connected
                        s_samples_dropped += accel_batch_count;
                    }

                    // Reset batch
                    accel_batch_count = 0;
                }
            }
        }

        // =====================================================================
        // Read ALL available SCL3300 samples from ring buffer
        // =====================================================================
        while (scl3300_data_available()) {
            if (scl3300_read_sample(&scl_sample)) {
                // Convert to DEGREES (using correct 0.0055 deg/LSB factor)
                s_latest_incl_x = convert_scl3300_to_deg(scl_sample.raw_x);
                s_latest_incl_y = convert_scl3300_to_deg(scl_sample.raw_y);
                s_latest_incl_z = convert_scl3300_to_deg(scl_sample.raw_z);
                s_has_incl_data = true;
            }
        }

        // Sleep before next poll
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

    // Reset statistics
    s_samples_published = 0;
    s_packets_sent = 0;
    s_samples_dropped = 0;

    // Reset data flags
    s_has_incl_data = false;
    s_has_temp_data = false;

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
