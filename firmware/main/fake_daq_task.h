/**
 * @file fake_daq_task.h
 * @brief Fake DAQ Task for Testing MQTT Communication
 *
 * DELETE THIS FILE once your teammate's real DAQ task is ready!
 */

#ifndef FAKE_DAQ_TASK_H
#define FAKE_DAQ_TASK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the fake DAQ task
 *
 * Generates dummy sensor data and pushes to queue.
 * Use this to test MQTT communication without real sensors.
 *
 * @return ESP_OK on success
 */
esp_err_t fake_daq_task_init(void);

/**
 * @brief Stop the fake DAQ task
 *
 * @return ESP_OK on success
 */
esp_err_t fake_daq_task_stop(void);

#ifdef __cplusplus
}
#endif

#endif // FAKE_DAQ_TASK_H
