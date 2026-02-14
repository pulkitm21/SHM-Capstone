/**
 * @file data_processing_and_mqtt_task.c
 * @brief SIMPLIFIED MQTT Task - Sends 1 sample at a time for debugging
 */

#include "data_processing_and_mqtt_task.h"
#include "sensor_types.h"
#include "mqtt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

static const char *TAG = "MQTT_TASK";

static QueueHandle_t s_sample_queue = NULL;
static TaskHandle_t s_mqtt_task_handle = NULL;
static volatile bool s_task_running = false;

static volatile uint32_t s_samples_published = 0;
static volatile uint32_t s_packets_sent = 0;
static volatile uint32_t s_samples_dropped = 0;

/*******************************************************************************
 * Unit Conversion Functions
 ******************************************************************************/

static inline float convert_accel_to_g(int32_t raw)
{
    return (float)raw * ACCEL_SCALE_2G;
}

static inline float convert_angle_to_deg(int16_t raw)
{
    return (float)raw * ANGLE_SCALE;
}

static inline float convert_temp_to_celsius(int16_t raw)
{
    return (float)raw * TEMP_SCALE;
}

/*******************************************************************************
 * MQTT Publishing Task - SIMPLIFIED (1 sample at a time)
 ******************************************************************************/

static void mqtt_publish_task(void *pvParameters)
{
    ESP_LOGI(TAG, "MQTT task started - SIMPLIFIED (1 sample at a time)");

    raw_sample_t sample;
    mqtt_sensor_packet_t packet = {0};

    while (s_task_running) {
        // Wait for 1 sample
        if (xQueueReceive(s_sample_queue, &sample, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;  // Timeout, try again
        }

        // Check if MQTT is connected
        if (!mqtt_is_connected()) {
            ESP_LOGW(TAG, "MQTT not connected, dropping sample");
            s_samples_dropped++;
            continue;
        }

        // Build packet with just 1 sample
        packet.timestamp = sample.timestamp_us;
        packet.accel_count = 1;

        // Convert accelerometer
        packet.accel[0].x = convert_accel_to_g(sample.accel_x_raw);
        packet.accel[0].y = convert_accel_to_g(sample.accel_y_raw);
        packet.accel[0].z = convert_accel_to_g(sample.accel_z_raw);

        // Convert inclinometer
        if (sample.flags & SAMPLE_FLAG_HAS_ANGLE) {
            packet.has_angle = true;
            packet.angle_x = convert_angle_to_deg(sample.angle_x_raw);
            packet.angle_y = convert_angle_to_deg(sample.angle_y_raw);
            packet.angle_z = convert_angle_to_deg(sample.angle_z_raw);
        } else {
            packet.has_angle = false;
        }

        // Convert temperature
        if (sample.flags & SAMPLE_FLAG_HAS_TEMP) {
            packet.has_temp = true;
            packet.temperature = convert_temp_to_celsius(sample.temp_raw);
        } else {
            packet.has_temp = false;
        }

        // Log what we're sending (for debugging)
        ESP_LOGI(TAG, "Sending: accel=[%.4f, %.4f, %.4f]g, angle=[%.2f, %.2f, %.2f]deg, temp=%.2fC",
                 packet.accel[0].x, packet.accel[0].y, packet.accel[0].z,
                 packet.angle_x, packet.angle_y, packet.angle_z,
                 packet.temperature);

        // Publish
        esp_err_t ret = mqtt_publish_sensor_data(&packet);
        if (ret == ESP_OK) {
            s_samples_published++;
            s_packets_sent++;
        } else {
            ESP_LOGW(TAG, "Failed to publish: %s", esp_err_to_name(ret));
            s_samples_dropped++;
        }
    }

    ESP_LOGI(TAG, "MQTT publish task stopped");
    vTaskDelete(NULL);
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t mqtt_task_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT task (SIMPLIFIED - 1 sample mode)...");

    // Smaller queue for debugging
    s_sample_queue = xQueueCreate(10, sizeof(raw_sample_t));
    if (s_sample_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sample queue");
        return ESP_ERR_NO_MEM;
    }

    s_task_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        mqtt_publish_task,
        "mqtt_task",
        8192,
        NULL,
        MQTT_TASK_PRIORITY,
        &s_mqtt_task_handle,
        MQTT_TASK_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT task");
        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MQTT task started");
    return ESP_OK;
}

QueueHandle_t mqtt_task_get_queue(void)
{
    return s_sample_queue;
}

esp_err_t mqtt_task_stop(void)
{
    s_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_sample_queue != NULL) {
        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;
    }

    s_mqtt_task_handle = NULL;
    ESP_LOGI(TAG, "MQTT task stopped");
    return ESP_OK;
}

void mqtt_task_get_stats(uint32_t *samples_published,
                         uint32_t *packets_sent,
                         uint32_t *samples_dropped)
{
    if (samples_published) *samples_published = s_samples_published;
    if (packets_sent) *packets_sent = s_packets_sent;
    if (samples_dropped) *samples_dropped = s_samples_dropped;
}
