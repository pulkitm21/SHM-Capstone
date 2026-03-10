/**
 * @file mqtt.h
 * @brief MQTT Client API
 *
 * DEVICE IDENTIFICATION:
<<<<<<< HEAD
 * - Client ID and topics are generated at runtime from the ESP32's burned-in MAC address
 * - Format: wind_turbine_AABBCCDDEEFF  (Ethernet MAC, 6 bytes, hex, uppercase)
 * - Data topic:   wind_turbine/AABBCCDDEEFF/data
 * - Status topic: wind_turbine/AABBCCDDEEFF/status
 *
 * Each ESP32 node publishes to a topic that includes its own MAC address:
=======
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
>>>>>>> origin/main
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

<<<<<<< HEAD
=======
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

>>>>>>> origin/main
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
<<<<<<< HEAD
const char *mqtt_get_client_id(void);
const char *mqtt_get_topic_data(void);
=======

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
>>>>>>> origin/main
const char *mqtt_get_topic_status(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
