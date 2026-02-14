/**
 * @file fake_daq_task.c
 * @brief SIMPLIFIED Fake DAQ Task - ALL 3 SENSORS EVERY SAMPLE
 *
 * For easy debugging - sends accel, angle, and temp with every sample.
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

static uint32_t tick_counter = 0;

static void fake_daq_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Fake DAQ started - SIMPLIFIED (all sensors every sample)");

    QueueHandle_t queue = mqtt_task_get_queue();
    if (queue == NULL) {
        ESP_LOGE(TAG, "Queue not available!");
        vTaskDelete(NULL);
        return;
    }

    // Simulated sensor values
    float sim_angle_x = 0.5f;
    float sim_angle_y = 0.3f;
    float sim_temp = 21.5f;

    while (s_task_running) {
        raw_sample_t sample = {0};

        sample.timestamp_us = esp_timer_get_time();
        float t = (float)tick_counter * 0.01f;

        // =========================================
        // Accelerometer - EVERY SAMPLE
        // =========================================
        sample.accel_x_raw = (int32_t)(sinf(t) * 5000);
        sample.accel_y_raw = (int32_t)(cosf(t) * 5000);
        sample.accel_z_raw = (int32_t)(256000);  // ~1g

        // =========================================
        // Inclinometer - EVERY SAMPLE (for debugging)
        // =========================================
        sim_angle_x = 0.5f + 0.1f * sinf(t * 0.1f);
        sim_angle_y = 0.3f + 0.1f * cosf(t * 0.1f);

        sample.angle_x_raw = (int16_t)(sim_angle_x / ANGLE_SCALE);
        sample.angle_y_raw = (int16_t)(sim_angle_y / ANGLE_SCALE);
        sample.angle_z_raw = 0;
        sample.flags |= SAMPLE_FLAG_HAS_ANGLE;

        // =========================================
        // Temperature - EVERY SAMPLE (for debugging)
        // =========================================
        sim_temp = 21.5f + 0.5f * sinf(t * 0.05f);
        sample.temp_raw = (int16_t)(sim_temp / TEMP_SCALE);
        sample.flags |= SAMPLE_FLAG_HAS_TEMP;

        // Push to queue
        xQueueSend(queue, &sample, 0);

        tick_counter++;

        // Slow rate for debugging - 1 sample per second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Fake DAQ task stopped");
    vTaskDelete(NULL);
}

esp_err_t fake_daq_task_init(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  SIMPLIFIED FAKE DAQ (DEBUG MODE)");
    ESP_LOGI(TAG, "  Sending ALL 3 sensors every sample");
    ESP_LOGI(TAG, "  Rate: 1 sample per second");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    s_task_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        fake_daq_task,
        "fake_daq",
        4096,
        NULL,
        10,
        &s_fake_daq_handle,
        1
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
    vTaskDelay(pdMS_TO_TICKS(100));
    s_fake_daq_handle = NULL;
    ESP_LOGI(TAG, "Fake DAQ task stopped");
    return ESP_OK;
}
