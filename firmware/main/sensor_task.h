/**
 * @file sensor_task.h
 * @brief Timing-Critical Sensor Acquisition System
 *
 * Architecture designed for minimal jitter in data acquisition:
 *
 * 1. SINGLE hardware timer at the fastest sampling rate
 * 2. SINGLE high-priority acquisition task that:
 *    - Wakes on hardware timer interrupt
 *    - Reads ALL sensors synchronously (no bus contention)
 *    - Stores raw data to lock-free queue
 *    - Does NO logging, NO formatting, NO blocking operations
 * 3. SINGLE low-priority processing task that:
 *    - Reads from queue
 *    - Formats and logs data
 *    - Can be interrupted at any time without affecting acquisition
 *
 * This design ensures:
 * - Deterministic acquisition timing
 * - No priority inversion on shared buses
 * - No context switch overhead from multiple timer tasks
 * - Acquisition is decoupled from slow operations (logging, transmission)
 *
 * Reference:
 * - https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gptimer.html
 * - https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos.html
 */

#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Configuration
 ******************************************************************************/

/**
 * @brief Base sampling period in microseconds
 * 
 * This is the fundamental timing interval. The hardware timer fires at this rate.
 * All sensor sampling is derived from this base period using decimation counters.
 * 
 * Example: 10ms base period (100 Hz)
 * - Accelerometer: every 1 tick = 100 Hz
 * - Inclinometer: every 10 ticks = 10 Hz  
 * - Temperature: every 100 ticks = 1 Hz
 */
#define BASE_SAMPLE_PERIOD_US       10000   // 10ms = 100 Hz base rate

/**
 * @brief Decimation factors (how many base ticks between samples)
 * 
 * Sensor rate = Base rate / Decimation factor
 * - Accel: 100 Hz / 1 = 100 Hz
 * - Angle: 100 Hz / 10 = 10 Hz
 * - Temp:  100 Hz / 100 = 1 Hz
 */
#define ACCEL_DECIMATION            1       // Every tick = 100 Hz
#define ANGLE_DECIMATION            10      // Every 10 ticks = 10 Hz
#define TEMP_DECIMATION             100     // Every 100 ticks = 1 Hz

/**
 * @brief GPTimer resolution (1 MHz = 1 tick per microsecond)
 */
#define GPTIMER_RESOLUTION_HZ       1000000

/**
 * @brief Task priorities
 * 
 * Acquisition task MUST be highest priority to minimize jitter.
 * Processing task is low priority - can be preempted anytime.
 */
#define ACQUISITION_TASK_PRIORITY   (configMAX_PRIORITIES - 1)  // Highest
#define PROCESSING_TASK_PRIORITY    2                            // Low

/**
 * @brief Task stack sizes in bytes
 */
#define ACQUISITION_TASK_STACK_SIZE 4096
#define PROCESSING_TASK_STACK_SIZE  8192    // Larger for logging/formatting

/**
 * @brief Queue size (number of samples to buffer)
 * 
 * Should be large enough to handle processing delays without dropping samples.
 */
#define SENSOR_QUEUE_SIZE           64

/**
 * @brief Core affinity
 * 
 * Pin acquisition task to dedicated core for best determinism.
 * Processing task can float.
 */
#define ACQUISITION_TASK_CORE       1       // Pin to APP_CPU
#define PROCESSING_TASK_CORE        tskNO_AFFINITY


/*******************************************************************************
 * Data Structures
 ******************************************************************************/

/**
 * @brief Raw sensor sample data
 * 
 * This structure is filled by the acquisition task and passed to processing.
 * Contains raw values only - no formatting or conversion.
 */
typedef struct {
    // Timestamp (microseconds since boot, from hardware timer)
    int64_t timestamp_us;
    
    // Sample counter (monotonic, for detecting missed samples)
    uint32_t sample_number;
    
    // Flags indicating which sensors have new data this sample
    struct {
        uint8_t accel_valid : 1;
        uint8_t angle_valid : 1;
        uint8_t temp_valid  : 1;
        uint8_t reserved    : 5;
    } flags;
    
    // Accelerometer raw data (only valid if accel_valid)
    struct {
        float x;
        float y;
        float z;
    } accel;
    
    // Inclinometer raw data (only valid if angle_valid)
    struct {
        float x;
        float y;
        float z;
    } angle;
    
    // Temperature raw data (only valid if temp_valid)
    float temperature;
    
} sensor_sample_t;


/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize the sensor acquisition system
 *
 * Creates hardware timer, queue, and tasks.
 * Does NOT start acquisition - call sensor_acquisition_start() for that.
 *
 * @param[in] temp_sensor_available Set true if ADT7420 initialized successfully
 *
 * @return
 *     - ESP_OK: System initialized successfully
 *     - ESP_FAIL: Initialization failed
 */
esp_err_t sensor_acquisition_init(bool temp_sensor_available);

/**
 * @brief Start sensor acquisition
 *
 * Starts the hardware timer. Acquisition begins immediately.
 *
 * @return
 *     - ESP_OK: Acquisition started
 *     - ESP_FAIL: Failed to start
 */
esp_err_t sensor_acquisition_start(void);

/**
 * @brief Stop sensor acquisition
 *
 * Stops the hardware timer. Acquisition stops immediately.
 *
 * @return
 *     - ESP_OK: Acquisition stopped
 *     - ESP_FAIL: Failed to stop
 */
esp_err_t sensor_acquisition_stop(void);

/**
 * @brief Get acquisition statistics
 *
 * @param[out] samples_acquired Total samples acquired
 * @param[out] samples_dropped Samples dropped due to queue full
 * @param[out] max_acquisition_time_us Maximum time spent in acquisition (jitter indicator)
 */
void sensor_acquisition_get_stats(uint32_t *samples_acquired,
                                   uint32_t *samples_dropped,
                                   uint32_t *max_acquisition_time_us);

/**
 * @brief Reset acquisition statistics
 */
void sensor_acquisition_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_TASK_H