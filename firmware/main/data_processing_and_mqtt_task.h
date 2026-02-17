/**
 * @file data_processing_and_mqtt_task.h
 * @brief MQTT Publishing Task - Reads from sensor_task ring buffers
 *
 * DATA INTEGRITY:
 * - Invalid/stale data shows as "null" in JSON
 * - No data is ever silently replaced
 * - Error statistics tracked
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

#define ACCEL_SAMPLES_PER_BATCH     100
#define PROCESSING_INTERVAL_MS      50

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

esp_err_t data_processing_and_mqtt_task_init(void);
esp_err_t data_processing_and_mqtt_task_stop(void);

void data_processing_and_mqtt_task_get_stats(uint32_t *samples_published,
                                              uint32_t *packets_sent,
                                              uint32_t *samples_dropped);

/**
 * @brief Get error statistics for data integrity monitoring
 *
 * @param[out] incl_errors   Number of inclinometer read failures
 * @param[out] temp_errors   Number of temperature read failures
 * @param[out] incl_stale    Number of times inclinometer data was stale
 * @param[out] temp_stale    Number of times temperature data was stale
 */
void data_processing_and_mqtt_task_get_error_stats(uint32_t *incl_errors,
                                                    uint32_t *temp_errors,
                                                    uint32_t *incl_stale,
                                                    uint32_t *temp_stale);

#ifdef __cplusplus
}
#endif

#endif // DATA_PROCESSING_AND_MQTT_TASK_H
