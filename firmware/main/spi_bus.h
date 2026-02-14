/**
 * @file spi_bus.h
 * @brief SPI bus initialization for ADXL355 and SCL3300 sensors
 */

#ifndef SPI_BUS_H
#define SPI_BUS_H

#include "esp_err.h"
#include "driver/spi_master.h"

// ============== SPI Host Selection ==============
// SPI2_HOST is HSPI on ESP32. Keep consistent across project.
#define SPI_BUS_HOST            SPI2_HOST

// ============== SPI Pin Configuration ==============
// These must match your wiring on the perfboard.
#define SPI_MOSI_IO             2      // Master Out, Slave In
#define SPI_MISO_IO             15      // Master In, Slave Out
#define SPI_SCLK_IO             14      // Clock
#define SPI_CS_ADXL355_IO       5       // Chip Select for accelerometer
#define SPI_CS_SCL3300_IO       4       // Chip Select for inclinometer

// ============== SPI Clock Speed ==============
// ADXL355 supports up to ~10 MHz. 8 MHz is a robust default.
#define SPI_CLOCK_SPEED_HZ      (1*1000*1000)  // 1 MHz

// Keep transfers small + deterministic (ADXL355 accel read = 1 cmd + 9 bytes = 10 bytes)
#define SPI_MAX_TRANSFER_BYTES  32

/**
 * @brief Initialize the SPI bus (shared by ADXL355 and SCL3300).
 *
 * Must be called once before initializing SPI sensors.
 */
esp_err_t spi_bus_init(void);

/**
 * @brief Get the SPI host device used by this project.
 */
spi_host_device_t spi_bus_get_host(void);

#endif // SPI_BUS_H