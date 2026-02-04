/**
 * @file i2c_bus.c
 * @brief I2C bus initialization and utilities
 */

#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "I2C_BUS";

// Store the bus handle so other files can access it
static i2c_master_bus_handle_t i2c_bus_handle = NULL;


esp_err_t i2c_bus_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C bus...");
    
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    esp_err_t ret = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C bus initialized on SDA=%d, SCL=%d", 
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    return ESP_OK;
}


i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return i2c_bus_handle;
}