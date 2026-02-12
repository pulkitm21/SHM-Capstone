/**
 * @file fake_daq_task.c
 * @brief Fake DAQ Task for Testing MQTT Communication
 *
 * This generates dummy sensor data so you can test:
 * - Queue communication
 * - MQTT task
 * - JSON packaging
 * - Network transmission to Raspberry Pi
 *
 * DELETE THIS FILE once your teammate's real DAQ task is ready!
 */

#include "fake_daq_task.h"
#include "sensor_types.h"
#include "data_processing_and_mqtt_task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>

static const char *TAG = "FAKE_DAQ";

static TaskHandle_t s_fake_daq_handle = NULL;
static volatile bool s_task_running = false;

// Counters for decimation (same as real DAQ would use)
static uint32_t tick_counter = 0;

/*******************************************************************************
 * Fake DAQ Task
 *
 * Simulates sensor readings at realistic rates:
 * - Accelerometer: 2000 Hz
 * - Inclinometer: 10 Hz (every 200th tick)
 * - Temperature: 1 Hz (every 2000th tick, offset by 100)
 ******************************************************************************/

static void fake_daq_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Fake DAQ task started - generating dummy data");

    QueueHandle_t queue = mqtt_task_get_queue();
    if (queue == NULL) {
        ESP_LOGE(TAG, "Queue not available!");
        vTaskDelete(NULL);
        return;
    }

    // Simulated sensor values
    float sim_angle_x = 0.5f;   // Simulated tilt
    float sim_angle_y = 0.3f;
    float sim_temp = 21.5f;     // Simulated temperature

    while (s_task_running) {
        raw_sample_t sample = {0};

        // Timestamp
        sample.timestamp_us = esp_timer_get_time();
        sample.flags = 0;

        // =========================================
        // Accelerometer (every tick - 2000 Hz)
        // =========================================
        // Simulate vibration: small oscillation around 0g X/Y, 1g Z
        float t = (float)tick_counter * 0.001f;  // Time in "seconds"

        // Raw values (simulating ±2g range, 20-bit resolution)
        // At rest: X≈0, Y≈0, Z≈1g (which is ~256000 LSB)
        sample.accel_x_raw = (int32_t)(sinf(t * 50.0f) * 5000);      // Small vibration
        sample.accel_y_raw = (int32_t)(cosf(t * 50.0f) * 5000);      // Small vibration
        sample.accel_z_raw = (int32_t)(256000 + sinf(t * 10.0f) * 1000);  // ~1g with slight variation

        // =========================================
        // Inclinometer (every 200th tick - 10 Hz)
        // Starting at tick 0
        // =========================================
        if (tick_counter % 200 == 0) {
            // Simulate slow drift in angle
            sim_angle_x += 0.01f * sinf(t * 0.1f);
            sim_angle_y += 0.01f * cosf(t * 0.1f);

            // Convert to raw (182 LSB per degree)
            sample.angle_x_raw = (int16_t)(sim_angle_x / ANGLE_SCALE);
            sample.angle_y_raw = (int16_t)(sim_angle_y / ANGLE_SCALE);
            sample.angle_z_raw = 0;

            sample.flags |= SAMPLE_FLAG_HAS_ANGLE;
        }

        // =========================================
        // Temperature (every 2000th tick - 1 Hz)
        // Starting at tick 100 (OFFSET to avoid collision!)
        // =========================================
        if (tick_counter % 2000 == 100) {
            // Simulate slow temperature drift
            sim_temp += 0.01f * sinf(t * 0.05f);

            // Convert to raw (16 LSB per degree C)
            sample.temp_raw = (int16_t)(sim_temp / TEMP_SCALE);

            sample.flags |= SAMPLE_FLAG_HAS_TEMP;
        }

        // =========================================
        // Push to queue
        // =========================================
        if (xQueueSend(queue, &sample, 0) != pdTRUE) {
            // Queue full - this is okay, MQTT task will catch up
        }

        tick_counter++;

        // Delay to simulate 2000 Hz (500 µs)
        // Note: vTaskDelay minimum is 1 tick (usually 1ms), so we can't
        // actually hit 2000 Hz with vTaskDelay. For testing, we'll run
        // at ~100 Hz which is enough to test the pipeline.
        // Real DAQ task uses hardware timer, not vTaskDelay.
        vTaskDelay(pdMS_TO_TICKS(10));  // 100 Hz for testing
    }

    ESP_LOGI(TAG, "Fake DAQ task stopped");
    vTaskDelete(NULL);
}


/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t fake_daq_task_init(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  FAKE DAQ TASK FOR TESTING");
    ESP_LOGI(TAG, "  Generating dummy sensor data");
    ESP_LOGI(TAG, "  DELETE THIS once real DAQ is ready!");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    s_task_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        fake_daq_task,
        "fake_daq",
        4096,
        NULL,
        10,             // Medium priority for testing
        &s_fake_daq_handle,
        1               // Core 1 (like real DAQ would be)
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fake DAQ task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fake DAQ task started");
    return ESP_OK;
}


esp_err_t fake_daq_task_stop(void)
{
    s_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(100));  // Let task exit cleanly
    s_fake_daq_handle = NULL;

    ESP_LOGI(TAG, "Fake DAQ task stopped");
    return ESP_OK;
}
