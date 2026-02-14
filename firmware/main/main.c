#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "driver/gpio.h"

#include "i2c_bus.h"
#include "spi_bus.h"

#include "adt7420.h"
#include "adxl355.h"
#include "scl3300.h"

static const char *TAG = "main";

static void halt_forever(void)
{
    ESP_LOGE(TAG, "*** HALT: Critical sensor init failed ***");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void force_spi_cs_high_early(void)
{
    // Force all SPI chip-select lines HIGH before ANY SPI init or device init.
    // This prevents any device from accidentally being selected and corrupting MISO.
    gpio_set_direction(SPI_CS_ADXL355_IO, GPIO_MODE_OUTPUT);
    gpio_set_direction(SPI_CS_SCL3300_IO, GPIO_MODE_OUTPUT);

    gpio_set_level(SPI_CS_ADXL355_IO, 1);
    gpio_set_level(SPI_CS_SCL3300_IO, 1);

    // Let lines settle
    vTaskDelay(pdMS_TO_TICKS(2));
}

void app_main(void)
{
    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, " Sensor bring-up test (NO ETH) ");
    ESP_LOGI(TAG, "==============================");

    // ---- Init buses ----
    esp_err_t ret;

    // **IMPORTANT**: Ensure both SPI devices are deselected BEFORE initializing SPI bus
    force_spi_cs_high_early();

    ret = i2c_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        halt_forever();
    }

    ret = spi_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        halt_forever();
    }

    // ---- Init sensors ----
    ret = adt7420_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADT7420 init failed (CRITICAL): %s", esp_err_to_name(ret));
        halt_forever();
    }
    ESP_LOGI(TAG, "ADT7420 OK");

    // Belt + suspenders: ensure SCL3300 is NOT selected when reading ADXL355 IDs
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    ret = adxl355_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADXL355 init failed (CRITICAL): %s", esp_err_to_name(ret));
        halt_forever();
    }
    ESP_LOGI(TAG, "ADXL355 OK");

    // Belt + suspenders: ensure ADXL355 is NOT selected when initializing SCL3300
    gpio_set_level(SPI_CS_ADXL355_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    ret = scl3300_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCL3300 init failed (CRITICAL): %s", esp_err_to_name(ret));
        halt_forever();
    }
    ESP_LOGI(TAG, "SCL3300 OK");

    ESP_LOGI(TAG, "ALL SENSORS INITIALIZED ✅");

    // ---- Optional sanity reads in a slow loop ----
    while (1) {
        float temp_c = 0.0f;
        adxl355_accel_t acc = {0};
        scl3300_angle_t ang = {0};

        esp_err_t e1 = adt7420_read_temperature(&temp_c);
        esp_err_t e2 = adxl355_read_acceleration(&acc);
        esp_err_t e3 = scl3300_read_angle(&ang);

        if (e1 == ESP_OK) ESP_LOGI(TAG, "ADT7420: %.2f C", temp_c);
        else             ESP_LOGW(TAG, "ADT7420 read failed: %s", esp_err_to_name(e1));

        if (e2 == ESP_OK) ESP_LOGI(TAG, "ADXL355: x=%.5fg y=%.5fg z=%.5fg", acc.x, acc.y, acc.z);
        else              ESP_LOGW(TAG, "ADXL355 read failed: %s", esp_err_to_name(e2));

        if (e3 == ESP_OK) ESP_LOGI(TAG, "SCL3300: x=%.3f y=%.3f z=%.3f deg", ang.x, ang.y, ang.z);
        else              ESP_LOGW(TAG, "SCL3300 read failed: %s", esp_err_to_name(e3));

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}