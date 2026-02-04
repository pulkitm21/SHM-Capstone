/**
 * @file spi_bus.h
 * @brief SPI bus initialization for ADXL355 and SCL3300 sensors
 */

#ifndef SPI_BUS_H
#define SPI_BUS_H

#include "esp_err.h"
#include "driver/spi_master.h"

// ============== SPI Pin Configuration ==============
// Change these pins based on your actual wiring!
// Avoid pins used by Ethernet: 0, 12, 17, 18, 19, 21, 22, 23, 25, 26, 27

#define SPI_MOSI_IO             14      // Master Out, Slave In
#define SPI_MISO_IO             15      // Master In, Slave Out  
#define SPI_SCLK_IO             2       // Clock
#define SPI_CS_ADXL355_IO       4       // Chip Select for accelerometer
#define SPI_CS_SCL3300_IO       5       // Chip Select for inclinometer

#define SPI_CLOCK_SPEED_HZ      1000000 // 1 MHz (safe starting speed)

/**
 * @brief Initialize the SPI bus
 * 
 * Sets up the SPI2 (HSPI) bus with the configured pins.
 * Must be called before initializing any SPI sensors.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t spi_bus_init(void);

/**
 * @brief Get the SPI host device
 * 
 * Returns the SPI host so sensor drivers can add their devices to the bus.
 * 
 * @return The SPI host device (SPI2_HOST)
 */
spi_host_device_t spi_bus_get_host(void);

#endif // SPI_BUS_H