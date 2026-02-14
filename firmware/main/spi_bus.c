/**
 * @file spi_bus.c
 * @brief SPI bus initialization for ADXL355 and SCL3300 sensors
 */

#include "spi_bus.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SPI_BUS";
static bool spi_bus_initialized = false;

static void spi_force_all_cs_high(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << SPI_CS_ADXL355_IO) | (1ULL << SPI_CS_SCL3300_IO),
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    gpio_set_level(SPI_CS_ADXL355_IO, 1);
    gpio_set_level(SPI_CS_SCL3300_IO, 1);

    // Give lines time to settle before clocks start
    vTaskDelay(pdMS_TO_TICKS(2));
}

esp_err_t spi_bus_init(void)
{
    if (spi_bus_initialized) {
        ESP_LOGW(TAG, "SPI bus already initialized");
        return ESP_OK;
    }

    spi_force_all_cs_high();

    spi_bus_config_t bus_config = {
        .mosi_io_num = SPI_MOSI_IO,
        .miso_io_num = SPI_MISO_IO,
        .sclk_io_num = SPI_SCLK_IO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER_BYTES,
    };

    esp_err_t ret = spi_bus_initialize(SPI_BUS_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_bus_initialized = true;
    ESP_LOGI(TAG, "SPI bus initialized: host=%d MOSI=%d MISO=%d SCLK=%d",
             (int)SPI_BUS_HOST, SPI_MOSI_IO, SPI_MISO_IO, SPI_SCLK_IO);

    return ESP_OK;
}

spi_host_device_t spi_bus_get_host(void)
{
    return SPI_BUS_HOST;
}