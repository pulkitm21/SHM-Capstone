/**
 * @file scl3300.c
 * @brief SCL3300-D01 Inclinometer Driver (SPI) - Angle measurement only
 */

#include "scl3300.h"
#include "spi_bus.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SCL3300";

// ============== SCL3300 Commands ==============
#define SCL3300_CMD_READ_ANG_X      0x240000C7
#define SCL3300_CMD_READ_ANG_Y      0x280000CD
#define SCL3300_CMD_READ_ANG_Z      0x2C0000CB
#define SCL3300_CMD_READ_STATUS     0x180000E5
#define SCL3300_CMD_READ_WHOAMI     0x40000091
#define SCL3300_CMD_SET_MODE1       0xB400001F  // ±12°, 40 Hz
#define SCL3300_CMD_SW_RESET        0xB4002098

#define SCL3300_WHOAMI_VALUE        0x00C1

// SPI device handle
static spi_device_handle_t scl3300_spi_handle = NULL;

// Angle scale factor for Mode 1: 182 LSB/degree
static const float ANGLE_SCALE = 0.0055f;


/**
 * @brief Transfer a 32-bit command and receive response
 */
static esp_err_t scl3300_transfer(uint32_t cmd, uint32_t *response)
{
    spi_transaction_t trans = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 32,
        .rxlength = 32,
    };

    // SCL3300 expects MSB first
    trans.tx_data[0] = (cmd >> 24) & 0xFF;
    trans.tx_data[1] = (cmd >> 16) & 0xFF;
    trans.tx_data[2] = (cmd >> 8) & 0xFF;
    trans.tx_data[3] = cmd & 0xFF;

    esp_err_t ret = spi_device_transmit(scl3300_spi_handle, &trans);
    if (ret != ESP_OK) {
        return ret;
    }

    if (response != NULL) {
        *response = ((uint32_t)trans.rx_data[0] << 24) |
                    ((uint32_t)trans.rx_data[1] << 16) |
                    ((uint32_t)trans.rx_data[2] << 8) |
                    trans.rx_data[3];
    }

    return ESP_OK;
}


/**
 * @brief Read 16-bit data using a command
 * 
 * SCL3300 returns the previous command's data, so we send twice.
 */
static esp_err_t scl3300_read_data(uint32_t cmd, int16_t *data)
{
    uint32_t response = 0;

    // Send command twice (first gets previous data, second gets this data)
    esp_err_t ret = scl3300_transfer(cmd, &response);
    if (ret != ESP_OK) return ret;

    ret = scl3300_transfer(cmd, &response);
    if (ret != ESP_OK) return ret;

    // Extract 16-bit data from response
    *data = (int16_t)((response >> 8) & 0xFFFF);

    return ESP_OK;
}


esp_err_t scl3300_init(void)
{
    ESP_LOGI(TAG, "Initializing SCL3300 inclinometer...");

    // Configure SPI device
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = 2000000,          // 2 MHz
        .mode = 0,                          // CPOL=0, CPHA=0
        .spics_io_num = SPI_CS_SCL3300_IO,
        .queue_size = 1,
        .command_bits = 0,
        .address_bits = 0,
    };

    esp_err_t ret = spi_bus_add_device(spi_bus_get_host(), &dev_config, &scl3300_spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for device to be ready
    vTaskDelay(pdMS_TO_TICKS(25));

    // Software reset
    uint32_t response = 0;
    ret = scl3300_transfer(SCL3300_CMD_SW_RESET, &response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send reset command: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    // Set Mode 1 (±12°, best precision)
    ret = scl3300_transfer(SCL3300_CMD_SET_MODE1, &response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mode: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    // Verify WHO_AM_I
    ret = scl3300_transfer(SCL3300_CMD_READ_WHOAMI, &response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send WHO_AM_I command: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = scl3300_transfer(SCL3300_CMD_READ_STATUS, &response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t whoami = (response >> 8) & 0xFFFF;
    if (whoami != SCL3300_WHOAMI_VALUE) {
        ESP_LOGW(TAG, "Unexpected WHO_AM_I: 0x%04X (expected 0x%04X)", 
                 whoami, SCL3300_WHOAMI_VALUE);
    }

    ESP_LOGI(TAG, "SCL3300 initialized successfully (WHO_AM_I: 0x%04X)", whoami);
    return ESP_OK;
}


esp_err_t scl3300_read_angle(scl3300_angle_t *angle)
{
    if (angle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t raw_x = 0, raw_y = 0, raw_z = 0;
    esp_err_t ret;

    ret = scl3300_read_data(SCL3300_CMD_READ_ANG_X, &raw_x);
    if (ret != ESP_OK) return ret;

    ret = scl3300_read_data(SCL3300_CMD_READ_ANG_Y, &raw_y);
    if (ret != ESP_OK) return ret;

    ret = scl3300_read_data(SCL3300_CMD_READ_ANG_Z, &raw_z);
    if (ret != ESP_OK) return ret;

    // Convert to degrees
    angle->x = raw_x * ANGLE_SCALE;
    angle->y = raw_y * ANGLE_SCALE;
    angle->z = raw_z * ANGLE_SCALE;

    return ESP_OK;
}