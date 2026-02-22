/**
 * @file sensor_task.h
 * @brief ISR-Based Sensor Data Acquisition with Ring Buffers
 *
 * Design Philosophy (per advisor recommendation):
 * ================================================
 * 1. SINGLE ISR driven by GPTimer at high frequency (8000 Hz)
 * 2. Staggered sensor sampling to prevent bus conflicts
 * 3. Raw data collection ONLY in ISR
 * 4. Lock-free ring buffers for each sensor
 * 5. Minimal ISR overhead
 * 6. Processing happens OUTSIDE the ISR in separate tasks
 *
 * Usage Pattern:
 * ==============
 * 1. Initialize sensors using their respective init functions
 * 2. Call sensor_acquisition_init(temp_available)
 * 3. Call sensor_acquisition_start()
 * 4. In your processing task:
 *    - Check if data available: adxl355_data_available()
 *    - Read raw sample: adxl355_read_sample(&raw_sample)
 *    - Convert to real units using sensor driver conversion functions
 *    - Package and send via MQTT/Ethernet
 */

#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * RAW DATA STRUCTURES
 * 
 * These contain raw register values from sensors.
 * Convert to real units using the conversion functions in the sensor drivers.
 *****************************************************************************/

/**
 * @brief Raw ADXL355 sample (20-bit values, not yet converted to g)
 */
typedef struct {
    uint32_t tick;      // Timestamp in timer ticks (8000 Hz)
    int32_t raw_x;      // Raw X-axis (20-bit sign-extended to 32-bit)
    int32_t raw_y;      // Raw Y-axis
    int32_t raw_z;      // Raw Z-axis
} adxl355_raw_sample_t;

/**
 * @brief Raw SCL3300 sample (16-bit values, not yet converted to degrees or g)
 */
typedef struct {
    uint32_t tick;      // Timestamp in timer ticks (8000 Hz)
    int16_t raw_x;      // Raw X-axis (16-bit)
    int16_t raw_y;      // Raw Y-axis
    int16_t raw_z;      // Raw Z-axis
} scl3300_raw_sample_t;

/**
 * @brief Raw ADT7420 sample (13-bit value, not yet converted to °C)
 */
typedef struct {
    uint32_t tick;      // Timestamp in timer ticks (8000 Hz)
    uint16_t raw_temp;  // Raw temperature (13-bit in 16-bit container)
} adt7420_raw_sample_t;

/******************************************************************************
 * INITIALIZATION AND CONTROL
 *****************************************************************************/

/**
 * @brief Initialize the sensor acquisition system
 *
 * Sets up:
 * - GPTimer at 8000 Hz (125 us period)
 * - Ring buffers for each sensor
 * - ISR for data collection
 *
 * Must be called AFTER all sensors are initialized with their init functions.
 * Does NOT start acquisition: call sensor_acquisition_start() for that.
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
 * Starts the hardware timer. ISR begins collecting data immediately.
 *
 * @return
 *     - ESP_OK: Acquisition started
 *     - ESP_FAIL: Failed to start
 */
esp_err_t sensor_acquisition_start(void);

/**
 * @brief Stop sensor acquisition
 *
 * Stops the hardware timer. ISR stops immediately.
 * Ring buffer contents remain accessible.
 *
 * @return
 *     - ESP_OK: Acquisition stopped
 *     - ESP_FAIL: Failed to stop
 */
esp_err_t sensor_acquisition_stop(void);

/******************************************************************************
 * ADXL355 RING BUFFER ACCESS
 * 
 * Sample rate: 1000 Hz
 * Buffer size: 4096 samples (~2 seconds)
 *****************************************************************************/

/**
 * @brief Check if ADXL355 data is available in ring buffer
 * @return true if at least one sample is available
 */
bool adxl355_data_available(void);

/**
 * @brief Read one raw sample from ADXL355 ring buffer
 * 
 * @param[out] sample Pointer to structure to receive raw sample data
 * @return true if sample was read, false if buffer is empty
 */
bool adxl355_read_sample(adxl355_raw_sample_t *sample);

/**
 * @brief Get number of samples currently in ADXL355 ring buffer
 * @return Number of unread samples
 */
uint32_t adxl355_samples_available(void);

/**
 * @brief Get ADXL355 buffer overflow count
 * 
 * Increments when ISR tries to write but buffer is full.
 * Indicates processing is too slow.
 * 
 * @return Number of samples dropped due to overflow
 */
uint32_t adxl355_get_overflow_count(void);

/**
 * @brief Get total number of ADXL355 samples acquired since start
 * @return Total sample count
 */
uint32_t adxl355_get_sample_count(void);

/******************************************************************************
 * SCL3300 RING BUFFER ACCESS
 * 
 * Sample rate: 20 Hz
 * Buffer size: 128 samples (6 seconds)
 *****************************************************************************/

/**
 * @brief Check if SCL3300 data is available in ring buffer
 * @return true if at least one sample is available
 */
bool scl3300_data_available(void);

/**
 * @brief Read one raw sample from SCL3300 ring buffer
 * 
 * @param[out] sample Pointer to structure to receive raw sample data
 * @return true if sample was read, false if buffer is empty
 */
bool scl3300_read_sample(scl3300_raw_sample_t *sample);

/**
 * @brief Get number of samples currently in SCL3300 ring buffer
 * @return Number of unread samples
 */
uint32_t scl3300_samples_available(void);

/**
 * @brief Get SCL3300 buffer overflow count
 * @return Number of samples dropped due to overflow
 */
uint32_t scl3300_get_overflow_count(void);

/**
 * @brief Get total number of SCL3300 samples acquired since start
 * @return Total sample count
 */
uint32_t scl3300_get_sample_count(void);

/******************************************************************************
 * ADT7420 RING BUFFER ACCESS
 * 
 * Sample rate: 1 Hz
 * Buffer size: 16 samples (16 seconds)
 *****************************************************************************/

/**
 * @brief Check if ADT7420 data is available in ring buffer
 * @return true if at least one sample is available
 */
bool adt7420_data_available(void);

/**
 * @brief Read one raw sample from ADT7420 ring buffer
 * 
 * @param[out] sample Pointer to structure to receive raw sample data
 * @return true if sample was read, false if buffer is empty
 */
bool adt7420_read_sample(adt7420_raw_sample_t *sample);

/**
 * @brief Get number of samples currently in ADT7420 ring buffer
 * @return Number of unread samples
 */
uint32_t adt7420_samples_available(void);

/**
 * @brief Get ADT7420 buffer overflow count
 * @return Number of samples dropped due to overflow
 */
uint32_t adt7420_get_overflow_count(void);

/**
 * @brief Get total number of ADT7420 samples acquired since start
 * @return Total sample count
 */
uint32_t adt7420_get_sample_count(void);

/******************************************************************************
 * STATISTICS AND DIAGNOSTICS
 *****************************************************************************/

/**
 * @brief Get acquisition statistics
 *
 * @param[out] samples_acquired Total samples acquired across all sensors
 * @param[out] samples_dropped Total samples dropped due to buffer overflow
 * @param[out] max_acquisition_time_us Maximum ISR execution time (jitter indicator)
 */
void sensor_acquisition_get_stats(uint32_t *samples_acquired,
                                   uint32_t *samples_dropped,
                                   uint32_t *max_acquisition_time_us);

/**
 * @brief Reset acquisition statistics
 * 
 * Resets all counters (sample counts, overflow counts, etc.)
 */
void sensor_acquisition_reset_stats(void);

/**
 * @brief Get current timer tick count
 * 
 * @return Current tick count (increments at 8000 Hz)
 */
uint32_t get_tick_count(void);

/******************************************************************************
 * CONVERSION HELPER MACROS
 * 
 * Use these in your processing task to convert raw values to real units.
 * The actual conversion logic should use the functions from  sensor drivers.
 *****************************************************************************/

/**
 * Convert timer ticks to microseconds
 * Tick rate is 8000 Hz, so each tick = 125 us
 */
#define TICKS_TO_US(ticks)      ((ticks) * 125)

/**
 * Convert timer ticks to milliseconds
 */
#define TICKS_TO_MS(ticks)      ((ticks) * 125 / 1000)

/**
 * Convert timer ticks to seconds (floating point)
 */
#define TICKS_TO_SEC(ticks)     ((float)(ticks) * 125.0f / 1000000.0f)

#ifdef __cplusplus
}
#endif

#endif // SENSOR_TASK_H
