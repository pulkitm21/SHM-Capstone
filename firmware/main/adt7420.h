/**
 * @file adt7420.h
 * @brief ADT7420 Temperature Sensor Driver (I2C)
 */

#ifndef ADT7420_H
#define ADT7420_H

#include "esp_err.h"

// ADT7420 I2C Address (A0=A1=0)
#define ADT7420_I2C_ADDR        0x48

// ADT7420 Register Addresses
#define ADT7420_REG_TEMP_MSB    0x00
#define ADT7420_REG_TEMP_LSB    0x01
#define ADT7420_REG_STATUS      0x02
#define ADT7420_REG_CONFIG      0x03
#define ADT7420_REG_ID          0x0B    // Should read 0xCB

/**
 * @brief Initialize the ADT7420 sensor
 * @return ESP_OK on success
 */
esp_err_t adt7420_init(void);

/**
 * @brief Read temperature from ADT7420
 * @param temperature Pointer to store temperature in Celsius
 * @return ESP_OK on success
 */
esp_err_t adt7420_read_temperature(float *temperature);

#endif // ADT7420_H