/**
 * @file sensor_types.h
 * @brief Shared Data Structures for DAQ and MQTT Tasks
 *
 * This file defines the raw sample structure that both tasks use.
 * DAQ task pushes raw_sample_t to the queue.
 * MQTT task pulls raw_sample_t from the queue.
 *
 * IMPORTANT: Both tasks MUST use this same header file!
 */

#ifndef SENSOR_TYPES_H
#define SENSOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Sample Flags
 ******************************************************************************/

#define SAMPLE_FLAG_HAS_ANGLE   0x01    // Bit 0: Inclinometer data valid
#define SAMPLE_FLAG_HAS_TEMP    0x02    // Bit 1: Temperature data valid

/*******************************************************************************
 * Raw Sample Structure
 *
 * This is what DAQ task pushes to the queue.
 * Size: ~28 bytes per sample
 ******************************************************************************/

typedef struct {
    // Timestamp (always present)
    uint32_t timestamp_us;          // Microseconds since boot (from esp_timer_get_time())

    // Accelerometer - ADXL355 (always present, 2000 Hz)
    // Raw 20-bit signed values from sensor
    int32_t accel_x_raw;
    int32_t accel_y_raw;
    int32_t accel_z_raw;

    // Inclinometer - SCL3300 (only when flags & SAMPLE_FLAG_HAS_ANGLE, 10 Hz)
    // Raw 16-bit signed values from sensor
    int16_t angle_x_raw;
    int16_t angle_y_raw;
    int16_t angle_z_raw;

    // Temperature - ADT7420 (only when flags & SAMPLE_FLAG_HAS_TEMP, 1 Hz)
    // Raw 13-bit signed value from sensor
    int16_t temp_raw;

    // Flags indicating which optional fields are valid
    uint8_t flags;

} raw_sample_t;

/*******************************************************************************
 * Unit Conversion Constants
 *
 * Use these to convert raw values to engineering units.
 ******************************************************************************/

// ADXL355 Accelerometer (±2g range, 20-bit resolution)
// Scale factor = 3.9 µg/LSB = 0.0000039 g/LSB
#define ACCEL_SCALE_2G      0.0000039f

// ADXL355 Accelerometer (±4g range)
// Scale factor = 7.8 µg/LSB
#define ACCEL_SCALE_4G      0.0000078f

// ADXL355 Accelerometer (±8g range)
// Scale factor = 15.6 µg/LSB
#define ACCEL_SCALE_8G      0.0000156f

// SCL3300 Inclinometer (Mode 1: ±12°)
// Scale factor = 0.0055 deg/LSB
#define ANGLE_SCALE         0.0055f

// ADT7420 Temperature (13-bit resolution)
// Scale factor = 0.0625 °C/LSB
#define TEMP_SCALE          0.0625f

/*******************************************************************************
 * Batching Configuration
 ******************************************************************************/

#define SAMPLES_PER_BATCH   500     // Number of samples per MQTT message

/*******************************************************************************
 * Queue Configuration
 ******************************************************************************/

#define SAMPLE_QUEUE_SIZE   2000    // Queue holds 1 second of data at 2000 Hz

#ifdef __cplusplus
}
#endif

#endif // SENSOR_TYPES_H
