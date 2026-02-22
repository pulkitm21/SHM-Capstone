/**
 * @file mqtt.h
 * @brief MQTT Client API
 *
 * DEVICE IDENTIFICATION:
 * - Client ID and topics are generated at runtime from the ESP32's burned-in MAC address
  * - Format: wind_turbine_AABBCCDDEEFF  (Ethernet MAC, 6 bytes, hex, uppercase)
 * - Data topic:   wind_turbine/AABBCCDDEEFF/data
 * - Status topic: wind_turbine/AABBCCDDEEFF/status
 *
 * ============================================================================
 * SETUP GUIDE for Tony and Pulkit (Raspberry Pi subscriber)
 * ============================================================================
 *
 * Each ESP32 node publishes to a topic that includes its own MAC address:
 *
 *   wind_turbine/<MAC>/data    e.g. wind_turbine/AABBCCDDEEFF/data
 *   wind_turbine/<MAC>/status  e.g. wind_turbine/AABBCCDDEEFF/status
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
 * constants anymore. they are generated at runtime from the MAC address.
 */

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
 * @brief Return the generated client ID string.
 *        e.g. "wind_turbine_AABBCCDDEEFF"
 */
const char *mqtt_get_client_id(void);

/**
 * @brief Return the generated data topic string.
 *        e.g. "wind_turbine/AABBCCDDEEFF/data"
 */
const char *mqtt_get_topic_data(void);

/**
 * @brief Return the generated status topic string.
 *        e.g. "wind_turbine/AABBCCDDEEFF/status"
 */
const char *mqtt_get_topic_status(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H