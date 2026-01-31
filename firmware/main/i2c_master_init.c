#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"

#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_SDA_IO          GPIO_NUM_13
#define I2C_SCL_IO          GPIO_NUM_16
#define I2C_FREQ_HZ         100000

#define ADT7420_ADDR        0x48
#define REG_TEMP_MSB        0x00
#define REG_ID              0x0B

static const char *TAG = "ADT7420";

static void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
        .clk_flags = 0,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0));
}

static esp_err_t i2c_read_reg(uint8_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        I2C_MASTER_NUM,
        dev,
        &reg, 1,
        data, len,
        pdMS_TO_TICKS(100)
    );
}

static float adt7420_temp_c_from_raw(int16_t raw)
{
    // ADT7420 default is 16-bit temperature with 1/128 °C per LSB
    // raw is already sign-extended as int16_t.
    return (float)raw / 128.0f;
}

void app_main(void)
{
    i2c_master_init();

    // 1) Read ID register
    uint8_t id = 0;
    esp_err_t err = i2c_read_reg(ADT7420_ADDR, REG_ID, &id, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ID register (0x0B) = 0x%02X (ADT7420 expected 0xCB)", id);
    } else {
        ESP_LOGE(TAG, "Failed to read ID register: %s", esp_err_to_name(err));
    }

    while (1) {
        // 2) Read temperature (2 bytes starting at 0x00)
        uint8_t buf[2] = {0};
        err = i2c_read_reg(ADT7420_ADDR, REG_TEMP_MSB, buf, 2);
        if (err == ESP_OK) {
            int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
            float temp_c = adt7420_temp_c_from_raw(raw);
            ESP_LOGI(TAG, "Temp raw=0x%04X  ->  %.2f C", (uint16_t)raw, temp_c);
        } else {
            ESP_LOGE(TAG, "Failed to read temperature: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
