/**
 * @file adt7420.c
 * @brief ADT7420 Temperature Sensor Driver (I2C)
 */

#include "adt7420.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "ADT7420";

// Device handle for the ADT7420
i2c_master_dev_handle_t adt7420_i2c_handle = NULL;  // Exposed for ISR
static i2c_master_dev_handle_t adt7420_handle = NULL; // Internal handle for driver functions


esp_err_t adt7420_init(void)
{
    ESP_LOGI(TAG, "Initializing ADT7420 temperature sensor.");
    
    // Get the I2C bus handle from i2c_bus module
    i2c_master_bus_handle_t bus_handle = i2c_bus_get_handle();
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized!");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Add ADT7420 device to the I2C bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADT7420_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &adt7420_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ADT7420 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    adt7420_i2c_handle = adt7420_handle;  // Expose handle for ISR access
    
    // Verify device ID (should be 0xCB)
    uint8_t reg_addr = ADT7420_REG_ID;
    uint8_t device_id = 0;
    
    ret = i2c_master_transmit_receive(adt7420_handle, &reg_addr, 1, &device_id, 1, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device ID: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (device_id != 0xCB) {
        ESP_LOGE(TAG, "Unexpected device ID: 0x%02X (expected 0xCB)", device_id);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    ESP_LOGI(TAG, "ADT7420 initialized successfully (ID: 0x%02X)", device_id);
    return ESP_OK;
}


esp_err_t adt7420_read_temperature(float *temperature)
{
    if (temperature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t reg_addr = ADT7420_REG_TEMP_MSB;
    uint8_t data[2] = {0};
    
    esp_err_t ret = i2c_master_transmit_receive(adt7420_handle, &reg_addr, 1, data, 2, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Convert to temperature (13-bit default resolution)
    int16_t raw_temp = (data[0] << 8) | data[1];
    raw_temp >>= 3;  // 13-bit resolution, shift out unused bits
    
    // Handle negative temperatures
    if (raw_temp & 0x1000) {
        raw_temp -= 8192;  // Sign extend for negative values
    }
    
    // Convert to Celsius (0.0625°C per LSB)
    *temperature = raw_temp * 0.0625f;
    
    return ESP_OK;
}