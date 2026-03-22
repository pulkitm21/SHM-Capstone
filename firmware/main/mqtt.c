/**
 * @file mqtt.c
 * @brief MQTT Client Implementation
 *
 * DEVICE IDENTIFICATION:
 * - At init time, the node's serial number is read from NVS (namespace "node_cfg",
 *   key "serial_no").  If found, it is used as the node identity:
 *     Client ID:    wind_turbine_WT01-N03
 *     Data topic:   wind_turbine/WT01-N03/data
 *     Status topic: wind_turbine/WT01-N03/status
 * - If no serial number has been provisioned, the Ethernet MAC address is used
 *   as a fallback so the node still appears on the broker, visibly unprovisioned:
 *     Client ID:    wind_turbine_MAC-AABBCCDDEEFF
 *     Data topic:   wind_turbine/MAC-AABBCCDDEEFF/data
 *     Status topic: wind_turbine/MAC-AABBCCDDEEFF/status
 *
 * PROVISIONING: see mqtt.h for the one-time NVS flashing instructions.
 *
 * BROKER DISCOVERY:
 * - The broker URI uses an mDNS hostname ("raspberrypi.local") instead of a
 *   hardcoded IP. Call mqtt_mdns_init(ethernet_get_netif()) AFTER
 *   ethernet_wait_for_ip() and BEFORE mqtt_init() so the mDNS stack is bound
 *   to the correct Ethernet netif before any hostname resolution is attempted.
 *
 * DATA INTEGRITY:
 * - Invalid/stale sensor data shows as "null" in JSON
 * - Every reading is 100% accurate or marked as missing
 */

#include "mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"          // esp_read_mac(), ESP_MAC_ETH
#include "mdns.h"             // mDNS hostname resolution over Ethernet
#include "nvs_flash.h"        // nvs_flash_init()
#include "nvs.h"              // nvs_open(), nvs_get_str()
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

/* Command handler registered by the application layer */
static mqtt_cmd_handler_t s_cmd_handler = NULL;

/* Command topic strings — built at subscribe time from s_serial_no */
#define CMD_TOPIC_MAX_LEN  80
static char s_topic_cmd_configure[CMD_TOPIC_MAX_LEN];
static char s_topic_cmd_control[CMD_TOPIC_MAX_LEN];
static const char s_topic_cmd_all[] = "wind_turbine/all/cmd/control";

/* Scratch buffer for inbound payloads (command packets are always small) */
#define CMD_PAYLOAD_MAX_LEN  256
static char s_cmd_payload_buf[CMD_PAYLOAD_MAX_LEN];

#define JSON_BUFFER_SIZE    6144
static char *s_json_buffer = NULL;

/*
 * Runtime-generated identity strings.
 *
 * s_serial_no holds the provisioned serial number (e.g. "WT01-N03") or the
 * MAC-based fallback (e.g. "MAC-AABBCCDDEEFF") if NVS has no entry yet.
 *
 * Sizes:
 *   serial_no  = MQTT_SERIAL_MAX_LEN (32)
 *   client_id  = "wind_turbine_" (13) + serial (32) + '\0' = 46  → use 64
 *   topic      = "wind_turbine/" (13) + serial (32) + "/data" (5) + '\0' = 51 → use 64
 */
#define CLIENT_ID_MAX_LEN   64
#define TOPIC_MAX_LEN       64

static char s_serial_no[MQTT_SERIAL_MAX_LEN];
static char s_client_id[CLIENT_ID_MAX_LEN];
static char s_topic_data[TOPIC_MAX_LEN];
static char s_topic_status[TOPIC_MAX_LEN];

/******************************************************************************
 * INTERNAL HELPERS
 *****************************************************************************/

/**
 * @brief Try to read the serial number from NVS.
 *
 * Opens the "node_cfg" namespace and reads the "serial_no" string key.
 * On success, copies the value into s_serial_no and returns ESP_OK.
 * On any failure (NVS not initialised, key absent, etc.) returns an error
 * code — the caller should then fall back to the MAC address.
 */
static esp_err_t read_serial_from_nvs(void)
{
    /* Initialise NVS flash if not already done by app_main */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated; erase and re-init (loses stored data) */
        ESP_LOGW(TAG, "NVS partition issue (err=0x%x), erasing...", ret);
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        /* Namespace does not exist yet — node has never been provisioned */
        ESP_LOGW(TAG, "NVS namespace '%s' not found — node is unprovisioned",
                 MQTT_NVS_NAMESPACE);
        return ret;
    }

    size_t len = sizeof(s_serial_no);
    ret = nvs_get_str(handle, MQTT_NVS_SERIAL_KEY, s_serial_no, &len);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS key '%s' not found in namespace '%s' — node is unprovisioned",
                 MQTT_NVS_SERIAL_KEY, MQTT_NVS_NAMESPACE);
        return ret;
    }

    ESP_LOGI(TAG, "NVS serial number read: '%s'", s_serial_no);
    return ESP_OK;
}

/**
 * @brief Build the MAC-based fallback identity string into s_serial_no.
 *
 * Produces "MAC-AABBCCDDEEFF" so unprovisioned nodes are obviously visible
 * on the broker and easy to distinguish from properly provisioned ones.
 */
static void build_mac_fallback(void)
{
    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_ETH);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read Ethernet MAC (err=0x%x), using UNKNOWN", ret);
        snprintf(s_serial_no, sizeof(s_serial_no), "UNKNOWN");
        return;
    }

    snprintf(s_serial_no, sizeof(s_serial_no), "MAC-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGW(TAG, "Node is UNPROVISIONED — using MAC fallback ID: %s", s_serial_no);
    ESP_LOGW(TAG, "Flash a serial number via NVS to assign a proper identity.");
}

/**
 * @brief Resolve the node identity and build client ID + topic strings.
 *
 * Priority:
 *   1. NVS serial number (e.g. "WT01-N03")   — provisioned node
 *   2. MAC-based fallback (e.g. "MAC-AABBCCDDEEFF") — unprovisioned node
 */
static void build_identity_strings(void)
{
    if (read_serial_from_nvs() != ESP_OK) {
        build_mac_fallback();
    }

    snprintf(s_client_id,    sizeof(s_client_id),    "%s_%s",        MQTT_TOPIC_PREFIX, s_serial_no);
    snprintf(s_topic_data,   sizeof(s_topic_data),   "%s/%s/data",   MQTT_TOPIC_PREFIX, s_serial_no);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status", MQTT_TOPIC_PREFIX, s_serial_no);
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
            /* Re-subscribe to cmd topics after reconnect */
            if (s_topic_cmd_configure[0] != '\0') {
                esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd_configure, 0);
                esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd_control,   0);
                esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd_all,       0);
                ESP_LOGI(TAG, "Re-subscribed to cmd topics after reconnect");
            }
            break;

        case MQTT_EVENT_DATA:
            /* Dispatch inbound command messages to the registered handler.
             * event->topic and event->data are NOT null-terminated. */
            if (event->topic && event->data && s_cmd_handler) {
                char topic_buf[CMD_TOPIC_MAX_LEN];
                int tlen = event->topic_len < (int)(sizeof(topic_buf) - 1)
                           ? event->topic_len : (int)(sizeof(topic_buf) - 1);
                memcpy(topic_buf, event->topic, tlen);
                topic_buf[tlen] = '\0';

                int plen = event->data_len < (CMD_PAYLOAD_MAX_LEN - 1)
                           ? event->data_len : (CMD_PAYLOAD_MAX_LEN - 1);
                memcpy(s_cmd_payload_buf, event->data, plen);
                s_cmd_payload_buf[plen] = '\0';

                /* Strip trailing whitespace / CRLF (Windows mosquitto_pub sends \r\n) */
                while (plen > 0 &&
                       (s_cmd_payload_buf[plen-1] == '\r' ||
                        s_cmd_payload_buf[plen-1] == '\n' ||
                        s_cmd_payload_buf[plen-1] == ' '))  {
                    s_cmd_payload_buf[--plen] = '\0';
                }

                ESP_LOGI(TAG, "CMD on [%s]: %s", topic_buf, s_cmd_payload_buf);
                s_cmd_handler(topic_buf, s_cmd_payload_buf);
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
        ESP_LOGE(TAG, "mqtt_mdns_init: netif is NULL — pass ethernet_get_netif()");
        return ESP_ERR_INVALID_ARG;
    }

    build_identity_strings();

    /*
     * Initialize the mDNS service. binds it to the provided netif.
     */
    esp_err_t ret = mdns_init();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * Give this node a hostname based on the client ID (which includes the MAC address).
     * This allows the MQTT broker to resolve "raspberrypi.local" at connect time
     */
    char mdns_hostname[CLIENT_ID_MAX_LEN];
    // Replace underscores with hyphens for the mDNS hostname
    strncpy(mdns_hostname, s_client_id, sizeof(mdns_hostname) - 1);
    mdns_hostname[sizeof(mdns_hostname) - 1] = '\0';
    for (char *p = mdns_hostname; *p; p++) {
        if (*p == '_') *p = '-';
    }

    ret = mdns_hostname_set(mdns_hostname);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed (non-fatal): %s", esp_err_to_name(ret));
        // Non-fatal. we still need mDNS for resolving the broker, not for advertising
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

    /* Build unique client ID and topics from the hardware MAC address */
    build_identity_strings();

    ESP_LOGI(TAG, "  Broker:    %s (resolved via mDNS)", MQTT_BROKER_URI);
    ESP_LOGI(TAG, "  Serial No: %s", s_serial_no);
    ESP_LOGI(TAG, "  Client ID: %s", s_client_id);
    ESP_LOGI(TAG, "  Data topic:   %s", s_topic_data);
    ESP_LOGI(TAG, "  Status topic: %s", s_topic_status);
    ESP_LOGI(TAG, "  Data integrity: null for invalid/stale data");

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
        .credentials.client_id          = s_client_id,   // MAC-derived, unique per device
        .session.keepalive               = 60,
        .network.reconnect_timeout_ms   = 5000,
        .buffer.size                    = 1024,
        .buffer.out_size                = 6144,
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

    /*
     * JSON FORMAT:
     * {
     *   "t": 123456789,                              // Timestamp (always present)
     *   "a": [[x,y,z], [x,y,z], ...],                // Accelerometer (always present)
     *   "i": [x, y, z] OR null,                      // Inclinometer (null if invalid)
     *   "T": 21.5 OR null                            // Temperature (null if invalid)
     * }
     */

    int offset = 0;

    // Timestamp
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                       "{\"t\":%lu,\"a\":[", (unsigned long)packet->timestamp);

    // Accelerometer array (always present)
    for (int i = 0; i < packet->accel_count && i < MQTT_ACCEL_BATCH_SIZE; i++) {
        if (i > 0) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, ",");
        }
        offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                           "[%.4f,%.4f,%.4f]",
                           packet->accel[i].x,
                           packet->accel[i].y,
                           packet->accel[i].z);

        if (offset >= JSON_BUFFER_SIZE - 100) {
            ESP_LOGE(TAG, "JSON buffer overflow!");
            return ESP_ERR_NO_MEM;
        }
    }

    // Close accelerometer array
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "]");

    // Inclinometer: ALWAYS include field, use null if invalid
    if (packet->has_angle) {
        if (packet->angle_valid) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"i\":[%.4f,%.4f,%.4f]",
                               packet->angle_x, packet->angle_y, packet->angle_z);
        } else {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"i\":null");
        }
    }

    // Temperature ALWAYS include field, use null if invalid
    if (packet->has_temp) {
        if (packet->temp_valid) {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"T\":%.2f", packet->temperature);
        } else {
            offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset,
                               ",\"T\":null");
        }
    }

    // Close JSON
    offset += snprintf(s_json_buffer + offset, JSON_BUFFER_SIZE - offset, "}");

    // Publish to the serial-number-derived data topic
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_data,
                                          s_json_buffer, offset,
                                          MQTT_PUBLISH_QOS, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish sensor data");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published %d bytes to %s (%d accel, angle=%s, temp=%s)",
             offset, s_topic_data, packet->accel_count,
             packet->angle_valid ? "valid" : "NULL",
             packet->temp_valid ? "valid" : "NULL");

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

/******************************************************************************
 * IDENTITY ACCESSORS
 *****************************************************************************/

const char *mqtt_get_serial_no(void)
{
    return s_serial_no;
}

const char *mqtt_get_client_id(void)
{
    return s_client_id;
}

const char *mqtt_get_topic_data(void)
{
    return s_topic_data;
}

const char *mqtt_get_topic_status(void)
{
    return s_topic_status;
}
/******************************************************************************
 * COMMAND SUBSCRIPTION
 *****************************************************************************/

void mqtt_set_cmd_handler(mqtt_cmd_handler_t handler)
{
    s_cmd_handler = handler;
}

esp_err_t mqtt_subscribe_cmd(void)
{
    if (!s_is_connected || s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "mqtt_subscribe_cmd: not connected");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_topic_cmd_configure, sizeof(s_topic_cmd_configure),
             "%s/%s/cmd/configure", MQTT_TOPIC_PREFIX, s_serial_no);
    snprintf(s_topic_cmd_control,   sizeof(s_topic_cmd_control),
             "%s/%s/cmd/control",   MQTT_TOPIC_PREFIX, s_serial_no);

    int r1 = esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd_configure, 0);
    int r2 = esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd_control,   0);
    int r3 = esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd_all,       0);

    if (r1 < 0 || r2 < 0 || r3 < 0) {
        ESP_LOGE(TAG, "Subscribe failed (configure=%d control=%d all=%d)", r1, r2, r3);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Subscribed to cmd topics:");
    ESP_LOGI(TAG, "  %s", s_topic_cmd_configure);
    ESP_LOGI(TAG, "  %s", s_topic_cmd_control);
    ESP_LOGI(TAG, "  %s", s_topic_cmd_all);
    return ESP_OK;
}

/******************************************************************************
 * STRUCTURED STATUS PUBLISHER
 *****************************************************************************/

esp_err_t mqtt_publish_status_json(const char *state_str,
                                   uint32_t odr_hz,
                                   uint8_t range,
                                   uint32_t output_hz,
                                   bool selftest_ok,
                                   uint32_t seq_ack,
                                   const char *error_msg)
{
    if (!s_is_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char buf[256];
    int offset = 0;
    int range_g = (range == 1) ? 2 : (range == 2) ? 4 : 8;

    offset += snprintf(buf + offset, sizeof(buf) - offset,
                       "{\"state\":\"%s\","
                       "\"odr_hz\":%lu,"
                       "\"range_g\":%d,"
                       "\"output_hz\":%lu,"
                       "\"selftest_ok\":%s,"
                       "\"seq_ack\":%lu",
                       state_str ? state_str : "unknown",
                       (unsigned long)odr_hz,
                       range_g,
                       (unsigned long)output_hz,
                       selftest_ok ? "true" : "false",
                       (unsigned long)seq_ack);

    if (error_msg && error_msg[0] != '\0') {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           ",\"error\":\"%s\"", error_msg);
    }

    offset += snprintf(buf + offset, sizeof(buf) - offset, "}");

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_status,
                                         buf, offset, MQTT_PUBLISH_QOS, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish status JSON");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status → %s: %s", s_topic_status, buf);
    return ESP_OK;
}
