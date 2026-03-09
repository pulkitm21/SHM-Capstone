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
 * Each ESP32 node publishes to a topic that includes its own MAC address:
 *
 *   wind_turbine/<MAC>/data    e.g. wind_turbine/AABBCCDDEEFF/data
 *   wind_turbine/<MAC>/status  e.g. wind_turbine/AABBCCDDEEFF/status
 *
 * To receive data from ALL nodes automatically, use:
 *
 *   Subscribe to:  wind_turbine/+/data
 *
 * TIMESTAMP FORMAT:
 * - All sensor readings carry their own PTP-synchronized timestamp.
 * - Timestamps are uint64_t microseconds since Unix epoch.
 * - On the Raspberry Pi: datetime.utcfromtimestamp(t / 1e6)
 * - Synchronized across all nodes via PTP (Raspberry Pi grandmaster).
 * - DO NOT use esp_timer_get_time() or FreeRTOS ticks for timestamps —
 *   those are not PTP-disciplined and will not be aligned between nodes.
 *
 * JSON FORMAT:
 * - {"a":[[t,x,y,z],...], "i":[t,x,y,z], "T":[t,val], "f":[...]}
 *   a = accelerometer samples, each with its own timestamp
 *   i = inclinometer [timestamp, x, y, z] or null
 *   T = temperature [timestamp, value] or null
 *   f = fault codes (optional, only present when faults exist)
 */

#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * CONFIGURATION
 *****************************************************************************/

#define MQTT_BROKER_HOSTNAME    "raspberrypi"
#define MQTT_BROKER_URI         "mqtt://" MQTT_BROKER_HOSTNAME ".local:1883"

#define MQTT_PUBLISH_QOS        0
#define MQTT_ACCEL_BATCH_SIZE   100

#define MQTT_TOPIC_PREFIX       "wind_turbine"

/******************************************************************************
 * DATA STRUCTURES
 *****************************************************************************/

/**
 * @brief Single accelerometer sample with per-sample timestamp and validity flag.
 *
 * timestamp = microseconds since Unix epoch, PTP-synchronized.
 * valid     = false means this sample was flagged as garbage and will be
 *             serialized as JSON null instead of [t, x, y, z].
 */
typedef struct {
    float x;
    float y;
    float z;
    bool valid;
    uint64_t timestamp;     /**< Microseconds since Unix epoch (PTP-synchronized) */
} mqtt_accel_sample_t;

/**
 * @brief Sensor data packet.
 *
 * ALL timestamps are microseconds since Unix epoch, PTP-synchronized.
 * On the Raspberry Pi: datetime.utcfromtimestamp(t / 1e6)
 *
 * JSON format:
 *   {"a":[[t,x,y,z],...], "i":[t,x,y,z], "T":[t,val]}
 *
 * DATA INTEGRITY RULES:
 *   - has_angle / has_temp:     whether to include the field in JSON at all
 *   - angle_valid / temp_valid: true = show values, false = show null
 *   - accel[i].valid:           per-sample flag; false = publish as null
 */
typedef struct {
    // Accelerometer (always present, individual samples may be null)
    mqtt_accel_sample_t accel[MQTT_ACCEL_BATCH_SIZE];
    int accel_count;

    // Inclinometer
    bool has_angle;             /**< Include "i" field in JSON? */
    bool angle_valid;           /**< true = show values, false = show null */
    float angle_x;
    float angle_y;
    float angle_z;
    uint64_t angle_timestamp;   /**< Microseconds since Unix epoch (PTP-synchronized) */

    // Temperature
    bool has_temp;              /**< Include "T" field in JSON? */
    bool temp_valid;            /**< true = show value, false = show null */
    float temperature;
    uint64_t temp_timestamp;    /**< Microseconds since Unix epoch (PTP-synchronized) */

} mqtt_sensor_packet_t;

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

esp_err_t mqtt_mdns_init(esp_netif_t *netif);
esp_err_t mqtt_init(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms);
esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet);
esp_err_t mqtt_publish_status(const char *status);
esp_err_t mqtt_publish(const char *topic, const char *data, int len);
esp_err_t mqtt_deinit(void);
const char *mqtt_get_client_id(void);
const char *mqtt_get_topic_data(void);
const char *mqtt_get_topic_status(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
