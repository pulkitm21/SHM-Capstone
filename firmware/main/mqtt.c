/**
 * @file mqtt.c
 * @brief MQTT Client Implementation
 *
 * DEVICE IDENTIFICATION:
 * - At init time, the ESP32 Ethernet MAC address is read via esp_read_mac().
 * - A unique client ID and per-node topics are built from those 6 MAC bytes:
 *     Client ID:    wind_turbine_AABBCCDDEEFF
 *     Data topic:   wind_turbine/AABBCCDDEEFF/data
 *     Status topic: wind_turbine/AABBCCDDEEFF/status
 *
 * BROKER DISCOVERY:
 * - The broker URI uses an mDNS hostname ("raspberrypi.local") instead of a
 *   hardcoded IP. Call mqtt_mdns_init(ethernet_get_netif()) AFTER
 *   ethernet_wait_for_ip() and BEFORE mqtt_init().
 *
 * TIMESTAMP FORMAT:
 * - All sensor readings carry their own PTP-synchronized timestamp.
 * - Timestamps are uint64_t microseconds since Unix epoch.
 * - On the Raspberry Pi: datetime.utcfromtimestamp(t / 1e6)
 *
 * JSON FORMAT:
 * - {"a":[[t,x,y,z],...], "i":[t,x,y,z], "T":[t,val]}
 *   a = accelerometer array, each sample [t, x, y, z] or null if invalid
 *   i = inclinometer [t, x, y, z] or null if invalid/unavailable
 *   T = temperature [t, value] or null if invalid/unavailable
 *   f = fault codes array (only present when faults exist)
 *
 * FAULT LOGGING:
 * - FAULT_MQTT_DISCONNECTED (4) logged on broker disconnect
 * - FAULT_MQTT_RECONNECTED  (5) logged on broker connect
 * - FAULT_MQTT_PUBLISH_FAIL (6) logged on publish failure
 */

#include "mqtt.h"
#include "fault_log.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
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

#define JSON_BUFFER_SIZE    4096
static char *s_json_buffer = NULL;

#define MAC_STR_LEN         12
#define CLIENT_ID_MAX_LEN   32
#define TOPIC_MAX_LEN       64

static char s_client_id[CLIENT_ID_MAX_LEN];
static char s_topic_data[TOPIC_MAX_LEN];
static char s_topic_status[TOPIC_MAX_LEN];

/******************************************************************************
 * INTERNAL HELPERS
 *****************************************************************************/

static void build_identity_strings(void)
{
    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_ETH);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read Ethernet MAC (err=0x%x), using fallback ID", ret);
        snprintf(s_client_id,    sizeof(s_client_id),    "%s_UNKNOWN", MQTT_TOPIC_PREFIX);
        snprintf(s_topic_data,   sizeof(s_topic_data),   "%s/UNKNOWN/data",   MQTT_TOPIC_PREFIX);
        snprintf(s_topic_status, sizeof(s_topic_status), "%s/UNKNOWN/status", MQTT_TOPIC_PREFIX);
        return;
    }

    char mac_str[MAC_STR_LEN + 1];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_client_id,    sizeof(s_client_id),    "%s_%s",         MQTT_TOPIC_PREFIX, mac_str);
    snprintf(s_topic_data,   sizeof(s_topic_data),   "%s/%s/data",    MQTT_TOPIC_PREFIX, mac_str);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status",  MQTT_TOPIC_PREFIX, mac_str);
}

/******************************************************************************
 * EVENT HANDLER
 *****************************************************************************/

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
            fault_log_record(FAULT_MQTT_RECONNECTED);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker");
            s_is_connected = false;
            if (s_mqtt_event_group) {
                xEventGroupSetBits(s_mqtt_event_group, MQTT_DISCONNECTED_BIT);
                xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
            }
            fault_log_record(FAULT_MQTT_DISCONNECTED);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TCP transport error");
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Message published, msg_id=%d", event->msg_id);
            break;

        default:
            break;
    }
}

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

esp_err_t mqtt_mdns_init(esp_netif_t *netif)
{
    if (netif == NULL) {
        ESP_LOGE(TAG, "mqtt_mdns_init: netif is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    build_identity_strings();

    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char mdns_hostname[CLIENT_ID_MAX_LEN];
    strncpy(mdns_hostname, s_client_id, sizeof(mdns_hostname) - 1);
    mdns_hostname[sizeof(mdns_hostname) - 1] = '\0';
    for (char *p = mdns_hostname; *p; p++) {
        if (*p == '_') *p = '-';
    }

    ret = mdns_hostname_set(mdns_hostname);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed (non-fatal): %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "mDNS hostname set: %s.local", mdns_hostname);
    }

    ESP_LOGI(TAG, "mDNS initialized — broker '%s' will be resolved at connect time",
             MQTT_BROKER_HOSTNAME);
    return ESP_OK;
}

esp_err_t mqtt_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT client...");

    build_identity_strings();

    ESP_LOGI(TAG, "  Broker:    %s (resolved via mDNS)", MQTT_BROKER_URI);
    ESP_LOGI(TAG, "  Client ID: %s", s_client_id);
    ESP_LOGI(TAG, "  Data topic:   %s", s_topic_data);
    ESP_LOGI(TAG, "  Status topic: %s", s_topic_status);

    s_mqtt_event_group = xEventGroupCreate();
    if (s_mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    s_json_buffer = malloc(JSON_BUFFER_SIZE);
    if (s_json_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        vEventGroupDelete(s_mqtt_event_group);
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri             = MQTT_BROKER_URI,
        .credentials.client_id          = s_client_id,
        .session.keepalive               = 60,
        .network.reconnect_timeout_ms   = 5000,
        .buffer.size                    = 1024,
        .buffer.out_size                = 4096,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        free(s_json_buffer);
        vEventGroupDelete(s_mqtt_event_group);
        return ESP_FAIL;
    }

    esp_err_t ret = esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler");
        esp_mqtt_client_destroy(s_mqtt_client);
        free(s_json_buffer);
        vEventGroupDelete(s_mqtt_event_group);
        return ret;
    }

    ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
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
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    return (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet)
{
    if (!s_is_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (packet == NULL || s_json_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int offset = 0;

    /* ------------------------------------------------------------------
     * Accelerometer array.
     * Each valid sample: [t, x, y, z]  (t = us since Unix epoch)
     * Invalid sample:    null
     * ------------------------------------------------------------------ */
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "{\"a\":[");

    for (int i = 0; i < packet->accel_count && i < MQTT_ACCEL_BATCH_SIZE; i++) {
        if (i > 0) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, ",");
        }

        if (packet->accel[i].valid) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               "[%llu,%.4f,%.4f,%.4f]",
                               (unsigned long long)packet->accel[i].timestamp,
                               packet->accel[i].x,
                               packet->accel[i].y,
                               packet->accel[i].z);
        } else {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "null");
        }

        if (offset >= JSON_BUFFER_SIZE - 100) {
            ESP_LOGE(TAG, "JSON buffer overflow!");
            return ESP_ERR_NO_MEM;
        }
    }

    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "]");

    /* ------------------------------------------------------------------
     * Inclinometer.
     * Valid:   "i":[t, x, y, z]
     * Invalid: "i":null
     * ------------------------------------------------------------------ */
    if (packet->has_angle) {
        if (packet->angle_valid) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"i\":[%llu,%.4f,%.4f,%.4f]",
                               (unsigned long long)packet->angle_timestamp,
                               packet->angle_x,
                               packet->angle_y,
                               packet->angle_z);
        } else {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"i\":null");
        }
    }

    /* ------------------------------------------------------------------
     * Temperature.
     * Valid:   "T":[t, value]
     * Invalid: "T":null
     * ------------------------------------------------------------------ */
    if (packet->has_temp) {
        if (packet->temp_valid) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"T\":[%llu,%.2f]",
                               (unsigned long long)packet->temp_timestamp,
                               packet->temperature);
        } else {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"T\":null");
        }
    }

    /* ------------------------------------------------------------------
     * Fault log (only appended when faults are pending).
     * ------------------------------------------------------------------ */
    if (fault_log_has_pending()) {
        offset = fault_log_append_to_json(s_json_buffer, JSON_BUFFER_SIZE, offset);
    }

    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "}");

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_data,
                                          s_json_buffer, offset,
                                          MQTT_PUBLISH_QOS, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish sensor data");
        fault_log_record(FAULT_MQTT_PUBLISH_FAIL);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published %d bytes to %s (%d accel, angle=%s, temp=%s)",
             offset, s_topic_data, packet->accel_count,
             packet->angle_valid ? "valid" : "NULL",
             packet->temp_valid  ? "valid" : "NULL");

    return ESP_OK;
}

esp_err_t mqtt_publish_status(const char *status)
{
    if (!s_is_connected || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_status,
                                          status, strlen(status),
                                          MQTT_PUBLISH_QOS, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Published status to %s: %s", s_topic_status, status);
    return ESP_OK;
}

esp_err_t mqtt_publish(const char *topic, const char *data, int len)
{
    if (!s_is_connected || topic == NULL || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len == 0) {
        len = strlen(data);
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, len,
                                          MQTT_PUBLISH_QOS, 0);

    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
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

const char *mqtt_get_client_id(void)    { return s_client_id; }
const char *mqtt_get_topic_data(void)   { return s_topic_data; }
const char *mqtt_get_topic_status(void) { return s_topic_status; }
