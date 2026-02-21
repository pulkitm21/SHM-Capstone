/**
 * @file mqtt.h
 * @brief MQTT Client API
 *
 * DEVICE IDENTIFICATION:
 * - Client ID and topics are generated at runtime from the ESP32's burned-in MAC address
  * - Format: wind_turbine_AABBCCDDEEFF  (Ethernet MAC, 6 bytes, hex, uppercase)
 * - Data topic:   wind_turbine/AABBCCDDEEFF/data
 * - Status topic: wind_turbine/AABBCCDDEEFF/status

 * This means each sensor node is automatically distinguishable — no manual ID config needed.
 *
 * DATA INTEGRITY:
 * - Invalid/stale data shows as "null" in JSON
 * - Every field has a validity flag
 * - No data is ever silently replaced
 *
 * ============================================================================
 * SETUP GUIDE for Tony and Pulkit (Python / Raspberry Pi subscriber)
 * ============================================================================
 *
 * Each ESP32 node publishes to a topic that includes its own MAC address:
 *
 *   wind_turbine/<MAC>/data    e.g. wind_turbine/AABBCCDDEEFF/data
 *   wind_turbine/<MAC>/status  e.g. wind_turbine/AABBCCDDEEFF/status
 *
 * To receive data from ALL nodes automatically, use MQTT wildcards:
 *
 *   Subscribe to:  wind_turbine/+/data
 *
 *   The "+" wildcard matches exactly one topic level, so it will match any
 *   node ID without you having to know the MAC address in advance.
 *   Every time a new sensor node comes online, your subscriber picks it up
 *   automatically. no code changes needed.
 *
 * Example Python snippet using paho-mqtt:
 *
 *   import paho.mqtt.client as mqtt
 *
 *   BROKER_IP = "192.168.0.112"   # <-- your broker's IP
 *   BROKER_PORT = 1883
 *
 *   def on_connect(client, userdata, flags, rc):
 *       print("Connected, rc =", rc)
 *       # Subscribe to all sensor nodes at once using the '+' wildcard
 *       client.subscribe("wind_turbine/+/data")
 *       client.subscribe("wind_turbine/+/status")
 *
 *   def on_message(client, userdata, msg):
 *       # Extract the node ID (MAC) from the topic string
 *       # Topic format: wind_turbine/<node_id>/data
 *       parts = msg.topic.split("/")
 *       node_id = parts[1]   # e.g. "AABBCCDDEEFF"
 *
 *       import json
 *       payload = json.loads(msg.payload.decode())
 *       print(f"Node {node_id}: {payload}")
 *
 *       # payload keys:
 *       #   "t"  -> timestamp (uint32, seconds since boot or Unix time)
 *       #   "a"  -> list of [x, y, z] accelerometer samples (up to 100)
 *       #   "i"  -> [x, y, z] inclinometer angles, or null if invalid
 *       #   "T"  -> temperature (float, Celsius), or null if invalid
 *
 *   client = mqtt.Client()
 *   client.on_connect = on_connect
 *   client.on_message = on_message
 *   client.connect(BROKER_IP, BROKER_PORT, keepalive=60)
 *   client.loop_forever()
 *
 * ============================================================================
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
 * BROKER HOSTNAME — resolved at runtime via mDNS, no hardcoded IP needed.
 *
 * "raspberrypi" is the default hostname on Raspberry Pi OS.
 * To verify your Pi's hostname, run:   hostname
 * in a terminal on the Pi. If it has been changed, update MQTT_BROKER_HOSTNAME below.
 *
 * HOW THIS WORKS:
 * The ESP32 uses mDNS (multicast DNS) to resolve "raspberrypi.local" to the
 * Pi's current IP address at connection time. This means the broker IP can
 * change (e.g. DHCP reassignment after a reboot) without any firmware changes.
 * mDNS works across a switch on a single LAN segment — exactly your setup.
 *
 * Tony: Ensure avahi-daemon is running on your Pi (it is by default on
 * Raspberry Pi OS, so you likely don't need to do anything):
 *   sudo systemctl status avahi-daemon    # check it's active
 *   sudo systemctl enable --now avahi-daemon  # enable if not already
 */
#define MQTT_BROKER_HOSTNAME    "raspberrypi"
#define MQTT_BROKER_URI         "mqtt://" MQTT_BROKER_HOSTNAME ".local:1883"

/*
 * MQTT_CLIENT_ID, MQTT_TOPIC_DATA, and MQTT_TOPIC_STATUS are NOT compile time
 * constants anymore. they are generated at runtime from the MAC address.
 * Use mqtt_get_client_id(), mqtt_get_topic_data(), mqtt_get_topic_status()
 * to read them after mqtt_init() has been called.
 */

#define MQTT_PUBLISH_QOS        0
#define MQTT_ACCEL_BATCH_SIZE   100

/* Fixed topic prefix — the node MAC is inserted between this and /data or /status */
#define MQTT_TOPIC_PREFIX       "wind_turbine"

/******************************************************************************
 * DATA STRUCTURES
 *****************************************************************************/

typedef struct {
    float x;
    float y;
    float z;
} mqtt_accel_sample_t;

/**
 * @brief Sensor data packet with validity flags
 *
 * DATA INTEGRITY RULES:
 * - has_angle/has_temp: Whether to include the field in JSON
 * - angle_valid/temp_valid: Whether data is valid (true) or null (false)
 * - If has_* is true but *_valid is false, JSON shows "null"
 */
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
 *
 * This binds the mDNS stack to the Ethernet interface so that the broker
 * hostname (e.g. "raspberrypi.local") can be resolved to an IP address when
 * the MQTT client connects. It also advertises this node on the network under
 * its own mDNS hostname (e.g. "wind-turbine-AABBCCDDEEFF.local"), which is
 * useful for debugging — you can ping individual sensor nodes by name from the Pi.
 *
 * Typical call order in main.c:
 *   ethernet_init();
 *   ethernet_wait_for_ip(10000);
 *   mqtt_mdns_init(ethernet_get_netif());
 *   mqtt_init();
 *   mqtt_wait_for_connection(10000);
 *
 * @param netif  The Ethernet netif handle (from ethernet_get_netif()).
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
 * @brief Return the generated client ID string (available after mqtt_init).
 *        e.g. "wind_turbine_AABBCCDDEEFF"
 */
const char *mqtt_get_client_id(void);

/**
 * @brief Return the generated data topic string (available after mqtt_init).
 *        e.g. "wind_turbine/AABBCCDDEEFF/data"
 */
const char *mqtt_get_topic_data(void);

/**
 * @brief Return the generated status topic string (available after mqtt_init).
 *        e.g. "wind_turbine/AABBCCDDEEFF/status"
 */
const char *mqtt_get_topic_status(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H