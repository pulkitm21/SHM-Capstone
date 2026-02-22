/**
 * @file scl3300.h
 * @brief SCL3300-D01 Inclinometer Driver (SPI) - Corrected Version
 *
 * Communication: SPI (datasheet commonly states up to ~4 MHz; tune if stable)
 * Resolution: 16-bit (angle output field)
 * 
 * This version includes correct SPI command frames from datasheet Table 15
 * 
 * Pin Configuration (from spi_bus.h):
 * - CS:   GPIO4  (SPI_CS_SCL3300_IO)
 * - MOSI: GPIO2  (shared SPI_MOSI_IO)
 * - MISO: GPIO15 (shared SPI_MISO_IO)
 * - SCLK: GPIO14 (shared SPI_SCLK_IO)
 * - SPI Mode: 0 (CPOL=0, CPHA=0)
 * - Clock: 2 MHz (recommended for best noise performance)
 * 
 * Hardware Requirements:
 * - VDD: 3.0-3.6V
 * - A_EXTC: 100nF capacitor to GND
 * - D_EXTC: 100nF capacitor to GND
 */

#ifndef SCL3300_H
#define SCL3300_H

#include "esp_err.h"
#include <stdint.h>
#include "driver/spi_master.h"
extern spi_device_handle_t scl3300_spi_handle;

#ifdef __cplusplus
extern "C" {
#endif

// Default per-device SPI clock for SCL3300 on the shared bus
// Using 2 MHz for optimal noise performance (datasheet recommendation)
#define SCL3300_SPI_CLOCK_HZ   (2 * 1000 * 1000)

// ============== SCL3300 Commands (Full 32-bit SPI frames with CRC) ==============
// These values are from datasheet Table 15 - do NOT modify

// Acceleration Data Read Commands
#define SCL3300_CMD_READ_ACC_X      0x040000F7u
#define SCL3300_CMD_READ_ACC_Y      0x080000FDu
#define SCL3300_CMD_READ_ACC_Z      0x0C0000FBu

// Angle Data Read Commands
#define SCL3300_CMD_READ_ANG_X      0x240000C7u
#define SCL3300_CMD_READ_ANG_Y      0x280000CDu
#define SCL3300_CMD_READ_ANG_Z      0x2C0000CBu

// Temperature and Status Commands
#define SCL3300_CMD_READ_TEMP       0x140000EFu
#define SCL3300_CMD_READ_STATUS     0x180000E5u

// Self-Test Commands
#define SCL3300_CMD_READ_STO        0x100000E9u

// Mode Selection Commands (Table 15)
#define SCL3300_CMD_SET_MODE1       0xB400001Fu  // Mode 1: 6000 LSB/g, 40 Hz
#define SCL3300_CMD_SET_MODE2       0xB4000102u  // Mode 2: 3000 LSB/g, 70 Hz
#define SCL3300_CMD_SET_MODE3       0xB4000225u  // Mode 3: 12000 LSB/g, 10 Hz (inclinometer)
#define SCL3300_CMD_SET_MODE4       0xB4000338u  // Mode 4: 12000 LSB/g, 10 Hz (low noise inclinometer)

// Power and Reset Commands
#define SCL3300_CMD_SET_POWERDOWN   0xB400046Bu
#define SCL3300_CMD_WAKE_UP         0xB400001Fu
#define SCL3300_CMD_SW_RESET        0xB4002098u

// Angle Control (Table 39 - Enable angle outputs)
#define SCL3300_CMD_ANG_CTRL_ENABLE 0xB0001F6Fu

// Identification Commands
#define SCL3300_CMD_READ_WHOAMI     0x40000091u
#define SCL3300_CMD_READ_SERIAL1    0x640000A7u
#define SCL3300_CMD_READ_SERIAL2    0x680000ADu

// Bank Selection Commands
#define SCL3300_CMD_READ_BANK       0x7C0000B3u
#define SCL3300_CMD_SWITCH_BANK0    0xFC000073u
#define SCL3300_CMD_SWITCH_BANK1    0xFC00016Eu

// Expected WHOAMI response
#define SCL3300_WHOAMI_VALUE        0x00C1u

// Return Status (RS) bits in SPI response [25:24]
#define SCL3300_RS_STARTUP          0x00u  // Startup in progress
#define SCL3300_RS_NORMAL           0x01u  // Normal operation, no flags
#define SCL3300_RS_RESERVED         0x02u  // Reserved
#define SCL3300_RS_ERROR            0x03u  // Error

// Data Structure for Angle Output
typedef struct {
    float x;    // X-axis angle in degrees
    float y;    // Y-axis angle in degrees
    float z;    // Z-axis angle in degrees
} scl3300_angle_t;

// Data Structure for Acceleration Output (if needed)
typedef struct {
    float x;    // X-axis acceleration in g
    float y;    // Y-axis acceleration in g
    float z;    // Z-axis acceleration in g
} scl3300_accel_t;

/**
 * @brief Initialize the SCL3300 inclinometer
 * 
 * This function performs the complete startup sequence from datasheet Table 11:
 * 1. Power-on delay (25ms)
 * 2. SW Reset
 * 3. Set measurement mode (Mode 1 by default)
 * 4. Enable angle outputs
 * 5. Signal path settling (25ms for Mode 1)
 * 6. Clear STATUS register (3x reads)
 * 7. Verify RS bits = '01'
 * 8. WHOAMI verification
 * 
 * Hardware connections (from spi_bus.h):
 * - CS:   GPIO4  (SPI_CS_SCL3300_IO)
 * - MOSI: GPIO2  (SPI_MOSI_IO  shared)
 * - MISO: GPIO15 (SPI_MISO_IO  shared)
 * - SCLK: GPIO14 (SPI_SCLK_IO  shared)
 * - VDD:  3.0-3.6V
 * - A_EXTC: 100nF capacitor to GND 
 * - D_EXTC: 100nF capacitor to GND 
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scl3300_init(void);

/**
 * @brief Read angle measurements from all three axes
 * 
 * Note: Angle outputs must be enabled via scl3300_init() or by calling
 * scl3300_enable_angles() separately.
 * 
 * @param angle Output structure for angle data (in degrees)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scl3300_read_angle(scl3300_angle_t *angle);

/**
 * @brief Read acceleration measurements from all three axes
 * 
 * @param accel Output structure for acceleration data (in g)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scl3300_read_accel(scl3300_accel_t *accel);

/**
 * @brief Enable angle outputs (normally done in init)
 * 
 * This writes to the ANG_CTRL register to enable angle calculations.
 * This is automatically done during scl3300_init(), but can be called
 * separately if needed.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scl3300_enable_angles(void);

/**
 * @brief Read and verify the WHOAMI register
 * 
 * @param whoami Output value (should be 0x00C1)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t scl3300_read_whoami(uint16_t *whoami);

#ifdef __cplusplus
}
#endif

#endif // SCL3300_H