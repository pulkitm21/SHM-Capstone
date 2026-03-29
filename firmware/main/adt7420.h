/**
 * @file adt7420.h
 * @brief ADT7420 Temperature Sensor Driver (I2C)
 */

#ifndef ADT7420_H
#define ADT7420_H

#include "esp_err.h"
#include "driver/i2c_master.h"

extern i2c_master_dev_handle_t adt7420_i2c_handle;

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

/**
 * @brief Power-on self-test: read temperature and verify it is physically plausible.
 *
 * Reads the STATUS register to confirm the sensor is present and a conversion
 * has completed, then reads temperature and checks it falls within the sensor's
 * rated operating range (-20 °C to +85 °C). A value outside this range
 * indicates a sensor fault or floating bus line.
 *
 * Call after adt7420_init() succeeds.
 *
 * @param[out] temperature_c  Measured temperature in °C.
 * @return ESP_OK if the read succeeded and the value is plausible.
 *         ESP_ERR_INVALID_RESPONSE if out of range or STATUS is 0xFF.
 */
esp_err_t adt7420_selftest(float *temperature_c);

#endif // ADT7420_H