/**
 * @file mqtt.h
 * @brief MQTT Client API
 *
 * DATA INTEGRITY:
 * - Invalid/stale data shows as "null" in JSON
 * - Every field has a validity flag
 * - No data is ever silently replaced
 */

#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * CONFIGURATION
 *****************************************************************************/

#define MQTT_BROKER_URI         "mqtt://192.168.0.112:1883"  // <-- UPDATE THIS!
#define MQTT_CLIENT_ID          "wind_turbine_esp32"
#define MQTT_TOPIC_DATA         "wind_turbine/data"
#define MQTT_TOPIC_STATUS       "wind_turbine/status"
#define MQTT_PUBLISH_QOS        0
#define MQTT_ACCEL_BATCH_SIZE   100

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

esp_err_t mqtt_init(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms);
esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet);
esp_err_t mqtt_publish_status(const char *status);
esp_err_t mqtt_publish(const char *topic, const char *data, int len);
esp_err_t mqtt_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
