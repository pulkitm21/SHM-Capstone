/**
 * @file mqtt.h
 * @brief MQTT Client API
 *
 * Handles connection to MQTT broker and publishing sensor data as JSON.
 */

#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// MQTT Broker URI - Change to your Raspberry Pi's IP address
#define MQTT_BROKER_URI         "mqtt://192.168.2.2:1883"

// Client identifier
#define MQTT_CLIENT_ID          "wind_turbine_esp32"

// Topics
#define MQTT_TOPIC_DATA         "wind_turbine/data"
#define MQTT_TOPIC_STATUS       "wind_turbine/status"

// QoS level (0 = at most once, 1 = at least once, 2 = exactly once)
#define MQTT_PUBLISH_QOS        0

// Maximum accelerometer samples per batch
#define MQTT_ACCEL_BATCH_SIZE   100

/******************************************************************************
 * DATA STRUCTURES
 *****************************************************************************/

/**
 * @brief Single 3-axis acceleration reading in g
 */
typedef struct {
    float x;
    float y;
    float z;
} mqtt_accel_sample_t;

/**
 * @brief Sensor data packet for MQTT publishing
 *
 * Contains batched accelerometer data plus latest inclinometer and temperature.
 */
typedef struct {
    uint32_t timestamp;                         // Timestamp in microseconds

    // Accelerometer data (batched)
    mqtt_accel_sample_t accel[MQTT_ACCEL_BATCH_SIZE];
    int accel_count;                            // Number of accel samples in this packet

    // Inclinometer data (latest reading)
    bool has_angle;
    float angle_x;                              // X-axis angle/accel
    float angle_y;                              // Y-axis angle/accel
    float angle_z;                              // Z-axis angle/accel

    // Temperature data (latest reading)
    bool has_temp;
    float temperature;                          // Temperature in Celsius

} mqtt_sensor_packet_t;

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

/**
 * @brief Initialize the MQTT client
 *
 * Connects to the broker specified in MQTT_BROKER_URI.
 * Connection happens asynchronously - use mqtt_wait_for_connection()
 * or mqtt_is_connected() to check status.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mqtt_init(void);

/**
 * @brief Check if connected to MQTT broker
 *
 * @return true if connected, false otherwise
 */
bool mqtt_is_connected(void);

/**
 * @brief Wait for MQTT connection with timeout
 *
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT if timeout
 */
esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms);

/**
 * @brief Publish sensor data as JSON
 *
 * Formats the packet as compact JSON and publishes to MQTT_TOPIC_DATA.
 *
 * JSON format:
 * {
 *   "t": 123456789,                    // Timestamp (microseconds)
 *   "a": [[x,y,z], [x,y,z], ...],      // Accelerometer array
 *   "i": [x, y, z],                    // Inclinometer (if has_angle)
 *   "T": 21.5                          // Temperature (if has_temp)
 * }
 *
 * @param packet Pointer to sensor data packet
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet);

/**
 * @brief Publish a status message
 *
 * Publishes a simple string to MQTT_TOPIC_STATUS.
 *
 * @param status Status message string
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mqtt_publish_status(const char *status);

/**
 * @brief Publish raw data to a custom topic
 *
 * @param topic Topic string
 * @param data Data to publish
 * @param len Length of data (0 to use strlen)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mqtt_publish(const char *topic, const char *data, int len);

/**
 * @brief Deinitialize the MQTT client
 *
 * Disconnects from broker and frees resources.
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
