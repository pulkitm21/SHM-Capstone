/**
 * @file mqtt.c
 * @brief SIMPLIFIED MQTT Client - Easy to read JSON for debugging
 */

#include "mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "MQTT";

#define MQTT_CONNECTED_BIT      BIT0
#define MQTT_DISCONNECTED_BIT   BIT1

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static EventGroupHandle_t s_mqtt_event_group = NULL;
static bool s_is_connected = false;

#define JSON_BUFFER_SIZE    1024
static char s_json_buffer[JSON_BUFFER_SIZE];

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
            break;

        default:
            break;
    }
}

esp_err_t mqtt_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT client...");
    ESP_LOGI(TAG, "  Broker: %s", MQTT_BROKER_URI);

    s_mqtt_event_group = xEventGroupCreate();
    if (s_mqtt_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        vEventGroupDelete(s_mqtt_event_group);
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    ESP_LOGI(TAG, "MQTT client started");
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
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    return (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet)
{
    if (!s_is_connected || packet == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Build SIMPLE JSON - easy to read for debugging
    // Format: {"t":123456,"a":[x,y,z],"i":[x,y,z],"T":21.5}

    int offset = 0;

    // Timestamp (microseconds since boot)
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                       "{\"t\":%lu", (unsigned long)packet->timestamp);

    // Accelerometer (just 1 sample: [x, y, z])
    if (packet->accel_count > 0) {
        offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                           ",\"a\":[%.4f,%.4f,%.4f]",
                           packet->accel[0].x,
                           packet->accel[0].y,
                           packet->accel[0].z);
    }

    // Inclinometer [x, y, z]
    if (packet->has_angle) {
        offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                           ",\"i\":[%.4f,%.4f,%.4f]",
                           packet->angle_x, packet->angle_y, packet->angle_z);
    }

    // Temperature
    if (packet->has_temp) {
        offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                           ",\"T\":%.2f", packet->temperature);
    }

    // Close JSON
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "}");

    // Publish
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_DATA,
                                          s_json_buffer, offset,
                                          MQTT_PUBLISH_QOS, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mqtt_publish_status(const char *status)
{
    if (!s_is_connected || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STATUS,
                            status, strlen(status), MQTT_PUBLISH_QOS, 0);
    return ESP_OK;
}

esp_err_t mqtt_publish(const char *topic, const char *data, int len)
{
    if (!s_is_connected) return ESP_ERR_INVALID_STATE;
    if (len == 0) len = strlen(data);
    esp_mqtt_client_publish(s_mqtt_client, topic, data, len, MQTT_PUBLISH_QOS, 0);
    return ESP_OK;
}

esp_err_t mqtt_deinit(void)
{
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    if (s_mqtt_event_group) {
        vEventGroupDelete(s_mqtt_event_group);
        s_mqtt_event_group = NULL;
    }
    s_is_connected = false;
    return ESP_OK;
}
