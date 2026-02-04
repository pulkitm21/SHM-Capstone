/**
 * @file spi_bus.c
 * @brief SPI bus initialization for ADXL355 and SCL3300 sensors
 */

#include "spi_bus.h"
#include "esp_log.h"

static const char *TAG = "SPI_BUS";

// Track if bus is initialized
static bool spi_bus_initialized = false;


esp_err_t spi_bus_init(void)
{
    if (spi_bus_initialized) {
        ESP_LOGW(TAG, "SPI bus already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SPI bus.");

    spi_bus_config_t bus_config = {
        .mosi_io_num = SPI_MOSI_IO,
        .miso_io_num = SPI_MISO_IO,
        .sclk_io_num = SPI_SCLK_IO,
        .quadwp_io_num = -1,        // Not used
        .quadhd_io_num = -1,        // Not used
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_bus_initialized = true;
    ESP_LOGI(TAG, "SPI bus initialized (MOSI=%d, MISO=%d, SCLK=%d)",
             SPI_MOSI_IO, SPI_MISO_IO, SPI_SCLK_IO);

    return ESP_OK;
}


spi_host_device_t spi_bus_get_host(void)
{
    return SPI2_HOST;
}