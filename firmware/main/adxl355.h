/**
 * @file adxl355.h
 * @brief ADXL355 Accelerometer Driver (SPI)
 * 
 * The ADXL355 is a 3-axis, low-noise accelerometer with
 * selectable measurement ranges (±2g, ±4g, ±8g).
 * 
 * Communication: SPI (up to 10 MHz)
 * Resolution: 20-bit
 */

#ifndef ADXL355_H
#define ADXL355_H

#include "esp_err.h"

// ADXL355 Register Addresses 
#define ADXL355_REG_DEVID_AD        0x00    // Device ID 
#define ADXL355_REG_DEVID_MST       0x01    // MEMS ID 
#define ADXL355_REG_PARTID          0x02    // Part ID 
#define ADXL355_REG_REVID           0x03    // Revision ID
#define ADXL355_REG_STATUS          0x04    // Status register
#define ADXL355_REG_FIFO_ENTRIES    0x05    // Number of FIFO entries
#define ADXL355_REG_TEMP2           0x06    // Temperature data [11:8]
#define ADXL355_REG_TEMP1           0x07    // Temperature data [7:0]
#define ADXL355_REG_XDATA3          0x08    // X-axis data [19:12]
#define ADXL355_REG_XDATA2          0x09    // X-axis data [11:4]
#define ADXL355_REG_XDATA1          0x0A    // X-axis data [3:0]
#define ADXL355_REG_YDATA3          0x0B    // Y-axis data [19:12]
#define ADXL355_REG_YDATA2          0x0C    // Y-axis data [11:4]
#define ADXL355_REG_YDATA1          0x0D    // Y-axis data [3:0]
#define ADXL355_REG_ZDATA3          0x0E    // Z-axis data [19:12]
#define ADXL355_REG_ZDATA2          0x0F    // Z-axis data [11:4]
#define ADXL355_REG_ZDATA1          0x10    // Z-axis data [3:0]
#define ADXL355_REG_FILTER          0x28    // Filter settings
#define ADXL355_REG_RANGE           0x2C    // Range setting
#define ADXL355_REG_POWER_CTL       0x2D    // Power control

// Range Settings 
#define ADXL355_RANGE_2G            0x01
#define ADXL355_RANGE_4G            0x02
#define ADXL355_RANGE_8G            0x03

// Power Control 
#define ADXL355_POWER_ON            0x00    // Measurement mode
#define ADXL355_POWER_STANDBY       0x01    // Standby mode

// Data Structure 
typedef struct {
    float x;    // X-axis acceleration in g
    float y;    // Y-axis acceleration in g
    float z;    // Z-axis acceleration in g
} adxl355_accel_t;

/**
 * @brief Initialize the ADXL355 accelerometer
 * 
 * Configures SPI communication, verifies device ID,
 * and sets default range (±2g).
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t adxl355_init(void);

/**
 * @brief Read acceleration from all three axes
 * 
 * @param accel Pointer to structure to store acceleration data (in g)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t adxl355_read_acceleration(adxl355_accel_t *accel);

/**
 * @brief Set the measurement range
 * 
 * @param range ADXL355_RANGE_2G, ADXL355_RANGE_4G, or ADXL355_RANGE_8G
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t adxl355_set_range(uint8_t range);

/**
 * @brief Read the internal temperature sensor
 * 
 * @param temperature Pointer to store temperature in Celsius
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t adxl355_read_temperature(float *temperature);

#endif // ADXL355_H