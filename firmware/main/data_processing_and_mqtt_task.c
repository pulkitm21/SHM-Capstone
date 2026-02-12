/**
 * @file mqtt_task.c
 * @brief MQTT Publishing Task Implementation
 *
 * Pulls raw samples from queue, converts units, packages JSON, publishes.
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

/*******************************************************************************
 * Private Variables
 ******************************************************************************/

static QueueHandle_t s_sample_queue = NULL;
static TaskHandle_t s_mqtt_task_handle = NULL;
static volatile bool s_task_running = false;

// Statistics
static volatile uint32_t s_samples_published = 0;
static volatile uint32_t s_packets_sent = 0;
static volatile uint32_t s_samples_dropped = 0;

/*******************************************************************************
 * Unit Conversion Functions
 ******************************************************************************/

/**
 * @brief Convert raw accelerometer value to g
 *
 * @param raw Raw 20-bit signed value from ADXL355
 * @return Acceleration in g
 */
static inline float convert_accel_to_g(int32_t raw)
{
    return (float)raw * ACCEL_SCALE_2G;
}

/**
 * @brief Convert raw inclinometer value to degrees
 *
 * @param raw Raw 16-bit signed value from SCL3300
 * @return Angle in degrees
 */
static inline float convert_angle_to_deg(int16_t raw)
{
    return (float)raw * ANGLE_SCALE;
}

/**
 * @brief Convert raw temperature value to Celsius
 *
 * @param raw Raw 13-bit signed value from ADT7420
 * @return Temperature in Celsius
 */
static inline float convert_temp_to_celsius(int16_t raw)
{
    // ADT7420: 13-bit resolution, already sign-extended by DAQ task
    return (float)raw * TEMP_SCALE;
}

/*******************************************************************************
 * MQTT Publishing Task
 ******************************************************************************/

static void mqtt_publish_task(void *pvParameters)
{
    ESP_LOGI(TAG, "MQTT publish task started");

    raw_sample_t samples[SAMPLES_PER_BATCH];
    mqtt_sensor_packet_t packet = {0};

    // Track latest angle and temperature (they update slower than accel)
    float latest_angle_x = 0.0f;
    float latest_angle_y = 0.0f;
    float latest_angle_z = 0.0f;
    bool has_angle = false;

    float latest_temp = 0.0f;
    bool has_temp = false;

    while (s_task_running) {
        // Wait for SAMPLES_PER_BATCH samples
        int samples_received = 0;

        while (samples_received < SAMPLES_PER_BATCH && s_task_running) {
            // Block waiting for sample (100ms timeout to check s_task_running)
            if (xQueueReceive(s_sample_queue, &samples[samples_received],
                              pdMS_TO_TICKS(100)) == pdTRUE) {
                samples_received++;
            }
        }

        if (!s_task_running) {
            break;
        }

        // Check if MQTT is connected
        if (!mqtt_is_connected()) {
            ESP_LOGW(TAG, "MQTT not connected, dropping %d samples", samples_received);
            s_samples_dropped += samples_received;
            continue;
        }

        // Build packet
        packet.timestamp = samples[0].timestamp_us;
        packet.accel_count = samples_received;

        // Convert all accelerometer samples
        for (int i = 0; i < samples_received; i++) {
            packet.accel[i].x = convert_accel_to_g(samples[i].accel_x_raw);
            packet.accel[i].y = convert_accel_to_g(samples[i].accel_y_raw);
            packet.accel[i].z = convert_accel_to_g(samples[i].accel_z_raw);

            // Update latest angle if this sample has it
            if (samples[i].flags & SAMPLE_FLAG_HAS_ANGLE) {
                latest_angle_x = convert_angle_to_deg(samples[i].angle_x_raw);
                latest_angle_y = convert_angle_to_deg(samples[i].angle_y_raw);
                latest_angle_z = convert_angle_to_deg(samples[i].angle_z_raw);
                has_angle = true;
            }

            // Update latest temperature if this sample has it
            if (samples[i].flags & SAMPLE_FLAG_HAS_TEMP) {
                latest_temp = convert_temp_to_celsius(samples[i].temp_raw);
                has_temp = true;
            }
        }

        // Add latest angle to packet
        packet.has_angle = has_angle;
        if (has_angle) {
            packet.angle_x = latest_angle_x;
            packet.angle_y = latest_angle_y;
            packet.angle_z = latest_angle_z;
        }

        // Add latest temperature to packet
        packet.has_temp = has_temp;
        if (has_temp) {
            packet.temperature = latest_temp;
        }

        // Publish!
        esp_err_t ret = mqtt_publish_sensor_data(&packet);
        if (ret == ESP_OK) {
            s_samples_published += samples_received;
            s_packets_sent++;

            ESP_LOGD(TAG, "Published %d samples (packet #%lu)",
                     samples_received, (unsigned long)s_packets_sent);
        } else {
            ESP_LOGW(TAG, "Failed to publish: %s", esp_err_to_name(ret));
            s_samples_dropped += samples_received;
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
    ESP_LOGI(TAG, "Initializing MQTT task...");

    // Create queue
    s_sample_queue = xQueueCreate(SAMPLE_QUEUE_SIZE, sizeof(raw_sample_t));
    if (s_sample_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sample queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sample queue created (size=%d, item=%d bytes)",
             SAMPLE_QUEUE_SIZE, sizeof(raw_sample_t));

    // Start task
    s_task_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        mqtt_publish_task,
        "mqtt_task",
        MQTT_TASK_STACK_SIZE,
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

    ESP_LOGI(TAG, "MQTT task started (priority=%d, core=%d)",
             MQTT_TASK_PRIORITY, MQTT_TASK_CORE);

    return ESP_OK;
}


QueueHandle_t mqtt_task_get_queue(void)
{
    return s_sample_queue;
}


esp_err_t mqtt_task_stop(void)
{
    ESP_LOGI(TAG, "Stopping MQTT task...");

    s_task_running = false;

    // Wait for task to finish (give it time to exit cleanly)
    vTaskDelay(pdMS_TO_TICKS(200));

    // Delete queue
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
