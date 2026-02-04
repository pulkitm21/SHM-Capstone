/**
 * @file scl3300.h
 * @brief SCL3300-D01 Inclinometer Driver (SPI)
 * 
 * The SCL3300 is a 3-axis inclinometer that provides
 * angle measurements.
 * 
 * Communication: SPI (up to 4 MHz)
 * Resolution: 16-bit
 */

#ifndef SCL3300_H
#define SCL3300_H

#include "esp_err.h"

// Data Structure 
typedef struct {
    float x;    // X-axis angle in degrees
    float y;    // Y-axis angle in degrees
    float z;    // Z-axis angle in degrees
} scl3300_angle_t;

/**
 * @brief Initialize the SCL3300 inclinometer
 * 
 * Configures SPI communication, performs software reset,
 * and verifies device ID.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scl3300_init(void);

/**
 * @brief Read angle from all three axes
 * 
 * @param angle Pointer to structure to store angle data (in degrees)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scl3300_read_angle(scl3300_angle_t *angle);

#endif // SCL3300_H