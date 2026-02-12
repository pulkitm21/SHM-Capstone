/**
 * @file mqtt.c
 * @brief MQTT Client Implementation
 *
 * Uses ESP-IDF MQTT client to publish sensor data to broker.
 * Data is formatted as compact JSON for efficient transmission.
 */

#include "mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "MQTT";

// Event bits for connection tracking
#define MQTT_CONNECTED_BIT      BIT0
#define MQTT_DISCONNECTED_BIT   BIT1

// MQTT client handle
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static EventGroupHandle_t s_mqtt_event_group = NULL;
static bool s_is_connected = false;

// JSON buffer for publishing (needs to be large for batched data)
// 500 samples * ~30 bytes per sample + overhead = ~16KB
#define JSON_BUFFER_SIZE    20000
static char *s_json_buffer = NULL;


/*******************************************************************************
 * Event Handler
 ******************************************************************************/

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT broker");
            s_is_connected = true;
            if (s_mqtt_event_group) {
                xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
                xEventGroupClearBits(s_mqtt_event_group, MQTT_DISCONNECTED_BIT);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker");
            s_is_connected = false;
            if (s_mqtt_event_group) {
                xEventGroupSetBits(s_mqtt_event_group, MQTT_DISCONNECTED_BIT);
                xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TCP transport error");
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            // Message published successfully (only for QoS > 0)
            ESP_LOGD(TAG, "Message published, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscribed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received data on topic: %.*s", event->topic_len, event->topic);
            break;

        default:
            ESP_LOGD(TAG, "Unhandled MQTT event: %ld", (long)event_id);
            break;
    }
}


/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t mqtt_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT client...");
    ESP_LOGI(TAG, "  Broker: %s", MQTT_BROKER_URI);
    ESP_LOGI(TAG, "  Client ID: %s", MQTT_CLIENT_ID);

    // Create event group
    s_mqtt_event_group = xEventGroupCreate();
    if (s_mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Allocate JSON buffer
    s_json_buffer = malloc(JSON_BUFFER_SIZE);
    if (s_json_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        vEventGroupDelete(s_mqtt_event_group);
        return ESP_ERR_NO_MEM;
    }

    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 1024,
        .buffer.out_size = 2048,
    };

    // Create client
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        free(s_json_buffer);
        vEventGroupDelete(s_mqtt_event_group);
        return ESP_FAIL;
    }

    // Register event handler
    esp_err_t ret = esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_mqtt_client);
        free(s_json_buffer);
        vEventGroupDelete(s_mqtt_event_group);
        return ret;
    }

    // Start client
    ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_mqtt_client);
        free(s_json_buffer);
        vEventGroupDelete(s_mqtt_event_group);
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started, waiting for connection...");
    return ESP_OK;
}


bool mqtt_is_connected(void)
{
    return s_is_connected;
}


esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms)
{
    if (s_mqtt_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group,
                                           MQTT_CONNECTED_BIT,
                                           pdFALSE,  // Don't clear on exit
                                           pdFALSE,  // Wait for any bit
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & MQTT_CONNECTED_BIT) {
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}


esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet)
{
    if (!s_is_connected) {
        ESP_LOGW(TAG, "Not connected to broker");
        return ESP_ERR_INVALID_STATE;
    }

    if (packet == NULL || s_json_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Build compact JSON
    // Format: {"t":timestamp,"a":[[x,y,z],[x,y,z],...],"i":[x,y,z],"T":temp}

    int offset = 0;

    // Start JSON object with timestamp
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                       "{\"t\":%lu,\"a\":[", (unsigned long)packet->timestamp);

    // Add accelerometer samples
    for (int i = 0; i < packet->accel_count && i < MQTT_ACCEL_BATCH_SIZE; i++) {
        if (i > 0) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, ",");
        }
        offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                           "[%.4f,%.4f,%.4f]",
                           packet->accel[i].x,
                           packet->accel[i].y,
                           packet->accel[i].z);

        // Safety check for buffer overflow
        if (offset >= JSON_BUFFER_SIZE - 100) {
            ESP_LOGE(TAG, "JSON buffer overflow!");
            return ESP_ERR_NO_MEM;
        }
    }

    // Close accelerometer array
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "]");

    // Add inclinometer data if available
    if (packet->has_angle) {
        offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                           ",\"i\":[%.4f,%.4f,%.4f]",
                           packet->angle_x, packet->angle_y, packet->angle_z);
    }

    // Add temperature if available
    if (packet->has_temp) {
        offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                           ",\"T\":%.2f", packet->temperature);
    }

    // Close JSON object
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "}");

    // Publish
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_DATA,
                                          s_json_buffer, offset,
                                          MQTT_PUBLISH_QOS, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish sensor data");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published %d bytes to %s", offset, MQTT_TOPIC_DATA);
    return ESP_OK;
}


esp_err_t mqtt_publish_status(const char *status)
{
    if (!s_is_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STATUS,
                                          status, strlen(status),
                                          MQTT_PUBLISH_QOS, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Published status: %s", status);
    return ESP_OK;
}


esp_err_t mqtt_publish(const char *topic, const char *data, int len)
{
    if (!s_is_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len == 0) {
        len = strlen(data);
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, len,
                                          MQTT_PUBLISH_QOS, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        return ESP_FAIL;
    }

    return ESP_OK;
}


esp_err_t mqtt_deinit(void)
{
    if (s_mqtt_client != NULL) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }

    if (s_json_buffer != NULL) {
        free(s_json_buffer);
        s_json_buffer = NULL;
    }

    if (s_mqtt_event_group != NULL) {
        vEventGroupDelete(s_mqtt_event_group);
        s_mqtt_event_group = NULL;
    }

    s_is_connected = false;
    ESP_LOGI(TAG, "MQTT client deinitialized");

    return ESP_OK;
}
