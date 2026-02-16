/**
 * @file adxl355.h
 * @brief ADXL355 Accelerometer Driver (SPI)
 *
 * Datasheet: ADXL354/ADXL355 Rev. D
 *
 * Notes:
 *  - SPI mode: CPOL=0, CPHA=0
 *  - SPI command byte (SPI protocol timing diagrams):
 *      bit7 = R/W   (1 = read, 0 = write)
 *      bit6 = MB    (1 = multibyte, 0 = single byte)
 *      bit5..0 = register address
 */

#ifndef ADXL355_H
#define ADXL355_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"
extern spi_device_handle_t adxl355_spi_handle;

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Register map (Table 15) ---------- */
#define ADXL355_REG_DEVID_AD        0x00
#define ADXL355_REG_DEVID_MST       0x01
#define ADXL355_REG_PARTID          0x02
#define ADXL355_REG_REVID           0x03
#define ADXL355_REG_STATUS          0x04
#define ADXL355_REG_FIFO_ENTRIES    0x05
#define ADXL355_REG_TEMP2           0x06
#define ADXL355_REG_TEMP1           0x07
#define ADXL355_REG_XDATA3          0x08
#define ADXL355_REG_XDATA2          0x09
#define ADXL355_REG_XDATA1          0x0A
#define ADXL355_REG_YDATA3          0x0B
#define ADXL355_REG_YDATA2          0x0C
#define ADXL355_REG_YDATA1          0x0D
#define ADXL355_REG_ZDATA3          0x0E
#define ADXL355_REG_ZDATA2          0x0F
#define ADXL355_REG_ZDATA1          0x10
#define ADXL355_REG_FIFO_DATA       0x11

#define ADXL355_REG_FILTER          0x28
#define ADXL355_REG_FIFO_SAMPLES    0x29
#define ADXL355_REG_INT_MAP         0x2A
#define ADXL355_REG_SYNC            0x2B
#define ADXL355_REG_RANGE           0x2C
#define ADXL355_REG_POWER_CTL       0x2D
#define ADXL355_REG_SELF_TEST       0x2E
#define ADXL355_REG_RESET           0x2F

/* ---------- Expected ID values (Table 15) ---------- */
#define ADXL355_DEVID_AD_EXPECTED   0xAD
#define ADXL355_DEVID_MST_EXPECTED  0x1D
#define ADXL355_PARTID_EXPECTED     0xED

/* ---------- POWER_CTL bits (Table 49) ---------- */
#define ADXL355_POWER_STANDBY_BIT   (1u << 0)
#define ADXL355_POWER_TEMP_OFF_BIT  (1u << 1)
#define ADXL355_POWER_DRDY_OFF_BIT  (1u << 2)

/* ---------- INT_MAP bits (Table 46) ---------- */
#define ADXL355_INT_RDY_EN1         (1u << 0)
#define ADXL355_INT_FULL_EN1        (1u << 1)
#define ADXL355_INT_OVR_EN1         (1u << 2)
#define ADXL355_INT_ACT_EN1         (1u << 3)
#define ADXL355_INT_RDY_EN2         (1u << 4)
#define ADXL355_INT_FULL_EN2        (1u << 5)
#define ADXL355_INT_OVR_EN2         (1u << 6)
#define ADXL355_INT_ACT_EN2         (1u << 7)

/* ---------- FILTER helpers (Table 44) ---------- */
#define ADXL355_FILTER_ODR_4000     0x00
#define ADXL355_FILTER_ODR_2000     0x01
#define ADXL355_FILTER_ODR_1000     0x02
#define ADXL355_FILTER_ODR_500      0x03
#define ADXL355_FILTER_ODR_250      0x04
#define ADXL355_FILTER_ODR_125      0x05
#define ADXL355_FILTER_ODR_62_5     0x06
#define ADXL355_FILTER_ODR_31_25    0x07
#define ADXL355_FILTER_ODR_15_625   0x08
#define ADXL355_FILTER_ODR_7_8125   0x09
#define ADXL355_FILTER_ODR_3_90625  0x0A

/* Range codes (Table 48) - lower two bits of RANGE register */
#define ADXL355_RANGE_2G            0x01
#define ADXL355_RANGE_4G            0x02
#define ADXL355_RANGE_8G            0x03

typedef struct {
    float x;    /**< X-axis acceleration (g) */
    float y;    /**< Y-axis acceleration (g) */
    float z;    /**< Z-axis acceleration (g) */
} adxl355_accel_t;

/**
 * @brief Initialize the ADXL355 over SPI.
 *
 * Performs (optional) soft reset, verifies device IDs, programs:
 *  - POWER_CTL to standby
 *  - FILTER (default: ODR=2000 Hz, HPF off)
 *  - RANGE  (default: ±2 g)
 * Then exits standby to measurement mode.
 */
esp_err_t adxl355_init(void);

/**
 * @brief Read acceleration (X, Y, Z) in g.
 */
esp_err_t adxl355_read_acceleration(adxl355_accel_t *accel);

/**
 * @brief Set measurement range (±2g/±4g/±8g).
 * @param range ADXL355_RANGE_2G / ADXL355_RANGE_4G / ADXL355_RANGE_8G
 */
esp_err_t adxl355_set_range(uint8_t range);

/**
 * @brief Read internal temperature in °C.
 */
esp_err_t adxl355_read_temperature(float *temperature_c);

#ifdef __cplusplus
}
#endif

#endif /* ADXL355_H */