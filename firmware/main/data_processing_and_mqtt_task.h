/**
 * @file mqtt_task.h
 * @brief MQTT Publishing Task
 *
 * This task:
 * 1. Pulls raw samples from the queue (filled by DAQ task)
 * 2. Converts raw values to engineering units (g, degrees, °C)
 * 3. Packages data as compact JSON
 * 4. Publishes via MQTT
 *
 * Runs at LOW PRIORITY so it never interferes with DAQ task.
 */

#ifndef MQTT_TASK_H
#define MQTT_TASK_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Configuration
 ******************************************************************************/

#define MQTT_TASK_PRIORITY      5           // Low priority (DAQ is ~24) CHANGE
#define MQTT_TASK_STACK_SIZE    8192        // 8KB stack (JSON needs space)
#define MQTT_TASK_CORE          0           // Run on core 0 (DAQ on core 1) CHANGE

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize the MQTT task
 *
 * Creates the sample queue and starts the MQTT publishing task.
 * Call this AFTER mqtt_init() and ethernet connection.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_task_init(void);

/**
 * @brief Get the sample queue handle
 *
 * DAQ task uses this to push samples to the queue.
 *
 * @return Queue handle, or NULL if not initialized
 */
QueueHandle_t mqtt_task_get_queue(void);

/**
 * @brief Stop the MQTT task
 *
 * Stops the task and deletes the queue.
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_task_stop(void);

/**
 * @brief Get statistics
 *
 * @param samples_published Pointer to store total samples published
 * @param packets_sent Pointer to store total MQTT packets sent
 * @param samples_dropped Pointer to store samples dropped (queue full)
 */
void mqtt_task_get_stats(uint32_t *samples_published,
                         uint32_t *packets_sent,
                         uint32_t *samples_dropped);

#ifdef __cplusplus
}
#endif

#endif // MQTT_TASK_H
