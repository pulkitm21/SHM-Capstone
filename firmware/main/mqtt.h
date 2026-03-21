/**
 * @file mqtt.h
 * @brief MQTT Client API
 *
 * DEVICE IDENTIFICATION:
 * - Each node has a human-readable serial number stored in NVS (non-volatile storage).
 * - Format: WT<turbine>-N<node>  e.g. WT01-N03  (Turbine 1, Node 3)
 * - If no serial number has been provisioned, the node falls back to its
 *   Ethernet MAC address so it still appears on the broker visibly as unprovisioned.
 *
 * - Data topic:   wind_turbine/<SERIAL>/data    e.g. wind_turbine/WT01-N03/data
 * - Status topic: wind_turbine/<SERIAL>/status  e.g. wind_turbine/WT01-N03/status
 *
 * ============================================================================
 * PROVISIONING A NODE (one-time setup per device)
 * ============================================================================
 *
 * Each ESP32 must be given its serial number once before deployment.
 * This is done by flashing a small NVS data partition over USB.
 *
 * Step 1 - Create a CSV file (e.g. nvs_serial.csv):
 *
 *   key,type,encoding,value
 *   node_cfg,namespace,,
 *   serial_no,data,string,WT01-N03
 *
 * Step 2 - Generate the binary NVS partition:
 *
 *   python3 $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
 *       generate nvs_serial.csv nvs_serial.bin 0x3000
 *
 * Step 3 - Flash it to the device (check your partition table for the NVS offset,
 *          typically 0x9000):
 *
 *   esptool.py --port /dev/ttyUSB0 write_flash 0x9000 nvs_serial.bin
 *
 * Step 4 - Power cycle the device. It will log its serial number on boot.
 *
 * ============================================================================
 * SETUP GUIDE for Tony and Pulkit (Raspberry Pi subscriber)
 * ============================================================================
 *
 * Each ESP32 node publishes to a topic that includes its serial number:
 *
 *   wind_turbine/<SERIAL>/data    e.g. wind_turbine/WT01-N03/data
 *   wind_turbine/<SERIAL>/status  e.g. wind_turbine/WT01-N03/status
 *
 * Unprovisioned nodes (no serial flashed yet) fall back to MAC-based topics:
 *
 *   wind_turbine/MAC-AABBCCDDEEFF/data
 *
 * To receive data from ALL nodes automatically, use:
 *
 *   Subscribe to:  wind_turbine/+/data
 */

#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include "esp_netif.h"   // esp_netif_t — needed for mqtt_mdns_init
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * CONFIGURATION
 *****************************************************************************/

/*
 * BROKER HOSTNAME: resolved at runtime via mDNS, no hardcoded IP needed.
 *
 * "raspberrypi" is the default hostname on Raspberry Pi OS supposedly
 */
#define MQTT_BROKER_HOSTNAME    "raspberrypi"
#define MQTT_BROKER_URI         "mqtt://" MQTT_BROKER_HOSTNAME ".local:1883"

/*
 * MQTT_CLIENT_ID, MQTT_TOPIC_DATA, and MQTT_TOPIC_STATUS are NOT
 * constants — they are generated at runtime from the NVS serial number
 * (or MAC address fallback if no serial has been provisioned).
 */

/* NVS namespace and key where the serial number is stored */
#define MQTT_NVS_NAMESPACE      "node_cfg"
#define MQTT_NVS_SERIAL_KEY     "serial_no"

/*
 * Maximum length of a serial number string (including null terminator).
 * Format: WT<turbine>-N<node>  e.g. "WT01-N03" = 8 chars + '\0' = 9.
 * 32 bytes gives plenty of headroom for longer site-specific schemes.
 */
#define MQTT_SERIAL_MAX_LEN     32

#define MQTT_PUBLISH_QOS        0
#define MQTT_ACCEL_BATCH_SIZE   100

/* Fixed topic prefix: the node MAC is inserted between this and /data or /status */
#define MQTT_TOPIC_PREFIX       "wind_turbine"

/******************************************************************************
 * DATA STRUCTURES
 *****************************************************************************/

typedef struct {
    float x;
    float y;
    float z;
} mqtt_accel_sample_t;

/*Sensor data packet with validity flags*/
typedef struct {
    uint32_t timestamp;

    // Accelerometer (always present)
    mqtt_accel_sample_t accel[MQTT_ACCEL_BATCH_SIZE];
    int accel_count;

    // Inclinometer
    bool has_angle;         // Include "i" field in JSON?
    bool angle_valid;       // true = show values, false = show null
    float angle_x;
    float angle_y;
    float angle_z;

    // Temperature
    bool has_temp;          // Include "T" field in JSON?
    bool temp_valid;        // true = show value, false = show null
    float temperature;

} mqtt_sensor_packet_t;

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

/**
 * @brief Initialise mDNS over the Ethernet netif for broker hostname resolution.
 *
 * MUST be called AFTER ethernet_wait_for_ip() and BEFORE mqtt_init().
 * Pass the netif handle from your ethernet layer: ethernet_get_netif().
 * call order in main.c:
 *   ethernet_init();
 *   ethernet_wait_for_ip(10000);
 *   mqtt_mdns_init(ethernet_get_netif());
 *   mqtt_init();
 *   mqtt_wait_for_connection(10000);
 *
 * @param netif  The Ethernet netif handle (from ethernet_get_netif()s).
 * @return ESP_OK on success, or an error code.
 */
esp_err_t mqtt_mdns_init(esp_netif_t *netif);

/**
 * @brief Initialise the MQTT client.
 *
 * Reads the ESP32 Ethernet MAC address, builds the client ID and topic
 * strings, then starts the MQTT client and begins connecting to the broker.
 * Call mqtt_wait_for_connection() afterwards if you need to block until ready.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t mqtt_init(void);

/** @brief Returns true if the client is currently connected to the broker. */
bool mqtt_is_connected(void);

/**
 * @brief Block until connected to the broker (or timeout).
 * @param timeout_ms Maximum wait time in milliseconds.
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms);

/** @brief Publish a structured sensor data packet. */
esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet);

/** @brief Publish a plain-text status string to the status topic. */
esp_err_t mqtt_publish_status(const char *status);

/** @brief Publish arbitrary data to an arbitrary topic. */
esp_err_t mqtt_publish(const char *topic, const char *data, int len);

/** @brief Stop and clean up the MQTT client. */
esp_err_t mqtt_deinit(void);

/**
 * @brief Subscribe to command topics for this node.
 *
 * Subscribes to:
 *   wind_turbine/<node_id>/cmd/configure
 *   wind_turbine/<node_id>/cmd/control
 *   wind_turbine/all/cmd/control
 *
 * Must be called after mqtt_wait_for_connection() succeeds.
 * Re-subscribed automatically on reconnect.
 *
 * @return ESP_OK on success.
 */
esp_err_t mqtt_subscribe_cmd(void);

/**
 * @brief Callback type invoked when a command packet is received.
 * @param topic   Null-terminated topic string
 * @param payload Null-terminated JSON payload
 */
typedef void (*mqtt_cmd_handler_t)(const char *topic, const char *payload);

/**
 * @brief Register the function to call when a cmd packet arrives.
 * Must be registered before mqtt_subscribe_cmd().
 */
void mqtt_set_cmd_handler(mqtt_cmd_handler_t handler);

/**
 * @brief Publish a structured status JSON to the status topic.
 *
 * Builds:
 *   {"state":"recording","odr_hz":1000,"range_g":2,"output_hz":200,
 *    "selftest_ok":true,"seq_ack":42}
 */
esp_err_t mqtt_publish_status_json(const char *state_str,
                                   uint32_t odr_hz,
                                   uint8_t range,
                                   uint32_t output_hz,
                                   bool selftest_ok,
                                   uint32_t seq_ack,
                                   const char *error_msg);

/**
 * @brief Return the serial number used for this node's identity.
 *        Either the NVS-provisioned value (e.g. "WT01-N03") or the
 *        MAC-based fallback (e.g. "MAC-AABBCCDDEEFF") if not provisioned.
 */
const char *mqtt_get_serial_no(void);

/**
 * @brief Return the generated client ID string.
 *        e.g. "wind_turbine_WT01-N03"
 */
const char *mqtt_get_client_id(void);

/**
 * @brief Return the generated data topic string.
 *        e.g. "wind_turbine/WT01-N03/data"
 */
const char *mqtt_get_topic_data(void);

/**
 * @brief Return the generated status topic string.
 *        e.g. "wind_turbine/WT01-N03/status"
 */
const char *mqtt_get_topic_status(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H