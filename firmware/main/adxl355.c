/**
 * @file adxl355.c
 * @brief ADXL355 Accelerometer Driver (SPI)
 */

#include "adxl355.h"
#include "spi_bus.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include <string.h>

static const char *TAG = "ADXL355";

// SPI device handle
static spi_device_handle_t adxl355_spi_handle = NULL;

// Current range setting (for conversion)
static uint8_t current_range = ADXL355_RANGE_2G;

// Scale factors for each range (LSB to g conversion)
// ADXL355 has 20-bit resolution, scale factor = range / 2^19
static const float scale_factor[] = {
    0.0000039f,     // ±2g: 3.9 µg/LSB
    0.0000078f,     // ±4g: 7.8 µg/LSB
    0.0000156f      // ±8g: 15.6 µg/LSB
};


/**
 * @brief Read a register from ADXL355
 */
static esp_err_t adxl355_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    spi_transaction_t trans = {
        .flags = 0,
        .cmd = 0,
        .addr = 0,
        .length = 8 * (1 + len),        // Command byte + data bytes
        .rxlength = 8 * (1 + len),
        .tx_buffer = NULL,
        .rx_buffer = NULL,
    };

    // Allocate buffers
    uint8_t *tx_buf = heap_caps_malloc(1 + len, MALLOC_CAP_DMA);
    uint8_t *rx_buf = heap_caps_malloc(1 + len, MALLOC_CAP_DMA);
    if (!tx_buf || !rx_buf) {
        free(tx_buf);
        free(rx_buf);
        return ESP_ERR_NO_MEM;
    }

    // First byte: register address with read bit (bit 0 = 1)
    tx_buf[0] = (reg << 1) | 0x01;
    memset(&tx_buf[1], 0, len);

    trans.tx_buffer = tx_buf;
    trans.rx_buffer = rx_buf;

    esp_err_t ret = spi_device_transmit(adxl355_spi_handle, &trans);
    if (ret == ESP_OK) {
        memcpy(data, &rx_buf[1], len);
    }

    free(tx_buf);
    free(rx_buf);
    return ret;
}


/**
 * @brief Write a register to ADXL355
 */
static esp_err_t adxl355_write_reg(uint8_t reg, uint8_t data)
{
    spi_transaction_t trans = {
        .flags = 0,
        .length = 16,   // 2 bytes: command + data
        .rxlength = 0,
        .tx_buffer = NULL,
        .rx_buffer = NULL,
    };

    uint8_t *tx_buf = heap_caps_malloc(2, MALLOC_CAP_DMA);
    if (!tx_buf) {
        return ESP_ERR_NO_MEM;
    }

    // First byte: register address with write bit (bit 0 = 0)
    tx_buf[0] = (reg << 1) | 0x00;
    tx_buf[1] = data;

    trans.tx_buffer = tx_buf;

    esp_err_t ret = spi_device_transmit(adxl355_spi_handle, &trans);

    free(tx_buf);
    return ret;
}


esp_err_t adxl355_init(void)
{
    ESP_LOGI(TAG, "Initializing ADXL355 accelerometer...");

    // Configure SPI device
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = SPI_CLOCK_SPEED_HZ,
        .mode = 0,                          // CPOL=0, CPHA=0
        .spics_io_num = SPI_CS_ADXL355_IO,
        .queue_size = 1,
        .command_bits = 0,
        .address_bits = 0,
    };

    esp_err_t ret = spi_bus_add_device(spi_bus_get_host(), &dev_config, &adxl355_spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify device ID
    uint8_t dev_id = 0;
    ret = adxl355_read_reg(ADXL355_REG_DEVID_AD, &dev_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device ID: %s", esp_err_to_name(ret));
        return ret;
    }

    if (dev_id != 0xAD) {
        ESP_LOGE(TAG, "Unexpected device ID: 0x%02X (expected 0xAD)", dev_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Verify part ID
    uint8_t part_id = 0;
    ret = adxl355_read_reg(ADXL355_REG_PARTID, &part_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read part ID: %s", esp_err_to_name(ret));
        return ret;
    }

    if (part_id != 0xED) {
        ESP_LOGE(TAG, "Unexpected part ID: 0x%02X (expected 0xED)", part_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Set default range (±2g)
    ret = adxl355_set_range(ADXL355_RANGE_2G);
    if (ret != ESP_OK) {
        return ret;
    }

    // Enable measurement mode (exit standby)
    ret = adxl355_write_reg(ADXL355_REG_POWER_CTL, ADXL355_POWER_ON);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable measurement mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ADXL355 initialized successfully (ID: 0x%02X, Part: 0x%02X)", dev_id, part_id);
    return ESP_OK;
}


esp_err_t adxl355_read_acceleration(adxl355_accel_t *accel)
{
    if (accel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Read all 9 bytes of acceleration data at once (X, Y, Z - 3 bytes each)
    uint8_t data[9] = {0};
    esp_err_t ret = adxl355_read_reg(ADXL355_REG_XDATA3, data, 9);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read acceleration: %s", esp_err_to_name(ret));
        return ret;
    }

    // Convert raw data to signed 20-bit values
    int32_t raw_x = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t raw_y = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t raw_z = ((int32_t)data[6] << 12) | ((int32_t)data[7] << 4) | (data[8] >> 4);

    // Sign extend from 20-bit to 32-bit
    if (raw_x & 0x80000) raw_x -= 0x100000;
    if (raw_y & 0x80000) raw_y -= 0x100000;
    if (raw_z & 0x80000) raw_z -= 0x100000;

    // Convert to g using scale factor
    float scale = scale_factor[current_range - 1];
    accel->x = raw_x * scale;
    accel->y = raw_y * scale;
    accel->z = raw_z * scale;

    return ESP_OK;
}


esp_err_t adxl355_set_range(uint8_t range)
{
    if (range < ADXL355_RANGE_2G || range > ADXL355_RANGE_8G) {
        ESP_LOGE(TAG, "Invalid range: %d", range);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = adxl355_write_reg(ADXL355_REG_RANGE, range);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set range: %s", esp_err_to_name(ret));
        return ret;
    }

    current_range = range;
    ESP_LOGI(TAG, "Range set to ±%dg", (1 << range));
    return ESP_OK;
}


esp_err_t adxl355_read_temperature(float *temperature)
{
    if (temperature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[2] = {0};
    esp_err_t ret = adxl355_read_reg(ADXL355_REG_TEMP2, data, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature: %s", esp_err_to_name(ret));
        return ret;
    }

    // Combine bytes into 12-bit value
    int16_t raw_temp = ((int16_t)(data[0] & 0x0F) << 8) | data[1];

    // Convert to Celsius
    // Formula from datasheet: Temp = ((raw - 1852) / -9.05) + 25
    *temperature = ((raw_temp - 1852) / -9.05f) + 25.0f;

    return ESP_OK;
}