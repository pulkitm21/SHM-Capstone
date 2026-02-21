/**
 * @file data_processing_and_mqtt_task.h
 * @brief MQTT Publishing Task - Reads from sensor_task ring buffers
 *
 * DATA INTEGRITY:
 * - If no data received → shows "null" in JSON
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

#define DATA_PROCESSING_TASK_STACK_SIZE    8192
#define DATA_PROCESSING_TASK_PRIORITY      5
#define DATA_PROCESSING_TASK_CORE          0

#define PROCESSING_INTERVAL_MS             50     // Process data every 50 ms
#define ACCEL_RAW_RATE_HZ                 1000   // Must match ADXL355 ODR
#define ACCEL_DECIM_FACTOR                 5    // Decimation factor to reduce 1000 Hz raw to 200 Hz published
#define ACCEL_RAW_SAMPLES_PER_INTERVAL  ((ACCEL_RAW_RATE_HZ * PROCESSING_INTERVAL_MS) / 1000)   // 50
#define ACCEL_SAMPLES_PER_BATCH         (ACCEL_RAW_SAMPLES_PER_INTERVAL / ACCEL_DECIM_FACTOR)   // 10

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

esp_err_t data_processing_and_mqtt_task_init(void);
esp_err_t data_processing_and_mqtt_task_stop(void);

void data_processing_and_mqtt_task_get_stats(uint32_t *samples_published,
                                              uint32_t *packets_sent,
                                              uint32_t *samples_dropped);

#ifdef __cplusplus
}
#endif

#endif // DATA_PROCESSING_AND_MQTT_TASK_H
