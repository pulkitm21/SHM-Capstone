/**
 * @file data_processing_and_mqtt_task.h
 * @brief MQTT Publishing Task - Reads from sensor_task ring buffers
 *
 * This task:
 * 1. Reads raw samples from ring buffers (adxl355, scl3300, adt7420)
 * 2. Converts raw values to engineering units (g, degrees, °C)
 * 3. Packages data as JSON
 * 4. Publishes to MQTT broker
 */

#ifndef DATA_PROCESSING_AND_MQTT_TASK_H
#define DATA_PROCESSING_AND_MQTT_TASK_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * CONFIGURATION
 *****************************************************************************/

// Task settings
#define DATA_PROCESSING_TASK_STACK_SIZE    8192
#define DATA_PROCESSING_TASK_PRIORITY      5
#define DATA_PROCESSING_TASK_CORE          0       // Run on core 0 (ISR runs on core 1)

// How many ADXL355 samples to batch per MQTT message
// At 2000 Hz, 100 samples = 50ms of data, ~20 messages/second
#define ACCEL_SAMPLES_PER_BATCH     100

// Processing interval (how often to check ring buffers)
#define PROCESSING_INTERVAL_MS      50

/******************************************************************************
 * UNIT CONVERSION CONSTANTS
 *****************************************************************************/

// ADXL355: ±2g range, 256000 LSB/g (from datasheet)
// Scale factor = 1/256000 = 3.9e-6 g/LSB
#define ADXL355_SCALE_FACTOR    (1.0f / 256000.0f)

// SCL3300 Mode 3: Inclinometer mode
// Sensitivity for acceleration: 18000 LSB/g
// Sensitivity for angle: 182 LSB/degree (approximately)
#define SCL3300_ACCEL_SCALE     (1.0f / 18000.0f)
#define SCL3300_ANGLE_SCALE     (1.0f / 182.0f)

// ADT7420: 13-bit resolution, 0.0625°C/LSB
#define ADT7420_TEMP_SCALE      0.0625f

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

/**
 * @brief Initialize and start the data processing and MQTT publishing task
 *
 * Creates a FreeRTOS task that:
 * - Polls sensor ring buffers
 * - Converts raw data to engineering units
 * - Batches and publishes via MQTT
 *
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t data_processing_and_mqtt_task_init(void);

/**
 * @brief Stop the data processing and MQTT publishing task
 *
 * @return ESP_OK on success
 */
esp_err_t data_processing_and_mqtt_task_stop(void);

/**
 * @brief Get task statistics
 *
 * @param[out] samples_published Total samples sent via MQTT
 * @param[out] packets_sent Total MQTT packets sent
 * @param[out] samples_dropped Samples dropped (MQTT disconnected)
 */
void data_processing_and_mqtt_task_get_stats(uint32_t *samples_published,
                                              uint32_t *packets_sent,
                                              uint32_t *samples_dropped);

#ifdef __cplusplus
}
#endif

#endif // DATA_PROCESSING_AND_MQTT_TASK_H
