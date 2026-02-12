/**
 * @file mqtt.h
 * @brief MQTT Client for Wind Turbine Sensor Data Transmission
 *
 * Handles connection to MQTT broker and publishing of batched sensor data.
 * Data is packaged as compact JSON for efficient transmission.
 *
 * Architecture:
 * - Receives processed sensor data from ring buffer
 * - Batches 500 accelerometer samples per message
 * - Publishes to configurable MQTT topics
 *
 * Reference:
 * - https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mqtt.html
 */

#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Configuration
 ******************************************************************************/

// MQTT Broker settings - UPDATE THESE FOR YOUR SETUP
#define MQTT_BROKER_URI         "mqtt://192.168.1.100:1883"  // Change to your Mac's IP
#define MQTT_CLIENT_ID          "wind_turbine_esp32"

// MQTT Topics
#define MQTT_TOPIC_DATA         "wind_turbine/data"
#define MQTT_TOPIC_STATUS       "wind_turbine/status"
#define MQTT_TOPIC_ACCEL        "wind_turbine/accel"
#define MQTT_TOPIC_ANGLE        "wind_turbine/angle"
#define MQTT_TOPIC_TEMP         "wind_turbine/temp"

// Batching settings
#define MQTT_ACCEL_BATCH_SIZE   500     // Samples per MQTT message
#define MQTT_PUBLISH_QOS        0       // QoS 0 = fire and forget (fastest)


/*******************************************************************************
 * Data Structures
 ******************************************************************************/

/**
 * @brief Accelerometer sample (compact format for batching)
 */
typedef struct {
    float x;
    float y;
    float z;
} mqtt_accel_sample_t;

/**
 * @brief Complete sensor data packet for MQTT transmission
 */
typedef struct {
    uint32_t timestamp;                             // Unix timestamp
    mqtt_accel_sample_t accel[MQTT_ACCEL_BATCH_SIZE]; // Batched accel data
    uint16_t accel_count;                           // Actual number of samples
    float angle_x;                                  // Inclinometer X
    float angle_y;                                  // Inclinometer Y
    float angle_z;                                  // Inclinometer Z
    float temperature;                              // Temperature in Celsius
    bool has_angle;                                 // Whether angle data is valid
    bool has_temp;                                  // Whether temp data is valid
} mqtt_sensor_packet_t;


/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize MQTT client
 *
 * Sets up MQTT client and connects to broker.
 * Requires Ethernet to be connected first.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_init(void);

/**
 * @brief Check if MQTT is connected to broker
 *
 * @return true if connected, false otherwise
 */
bool mqtt_is_connected(void);

/**
 * @brief Wait for MQTT connection
 *
 * Blocks until connected or timeout.
 *
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT if timeout
 */
esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms);

/**
 * @brief Publish sensor data packet
 *
 * Packages sensor data as compact JSON and publishes to MQTT broker.
 *
 * @param packet Pointer to sensor data packet
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publish_sensor_data(const mqtt_sensor_packet_t *packet);

/**
 * @brief Publish a simple status message
 *
 * @param status Status string to publish
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publish_status(const char *status);

/**
 * @brief Publish raw string to a topic
 *
 * @param topic MQTT topic
 * @param data Data string to publish
 * @param len Length of data (0 for null-terminated string)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publish(const char *topic, const char *data, int len);

/**
 * @brief Disconnect and cleanup MQTT client
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
