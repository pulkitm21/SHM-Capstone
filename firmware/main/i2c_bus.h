/**
 * @file i2c_bus.h
 * @brief I2C bus initialization and utilities
 */

#ifndef I2C_BUS_H
#define I2C_BUS_H

#include "esp_err.h"
#include "driver/i2c_master.h"

// I2C Configuration for Olimex ESP32-POE-ISO
#define I2C_MASTER_SDA_IO       13
#define I2C_MASTER_SCL_IO       16
#define I2C_MASTER_FREQ_HZ      100000    // 100kHz standard mode

/**
 * @brief Initialize the I2C master bus
 * @return ESP_OK on success
 */
esp_err_t i2c_bus_init(void);

/**
 * @brief Get the I2C bus handle (for adding devices)
 * @return The I2C bus handle
 */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

#endif // I2C_BUS_H