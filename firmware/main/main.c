/**
 * @file main.c
 * @brief Main application with ISR-based sensor acquisition
 * 
 * Workflow:
 * =========
 * 1. Initialize buses (I2C, SPI)
 * 2. Initialize sensors using their init functions
 * 3. Initialize ISR-based acquisition system
 * 4. Start acquisition
 * 5. Processing task reads from ring buffers and processes data
 */

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

#include "sensor_task.h"

static const char *TAG = "main";

/**
 * @brief Force SPI CS lines high before any initialization
 * 
 * This prevents accidental device selection during init
 */
static void force_spi_cs_high_early(void)
{
    gpio_set_direction(SPI_CS_ADXL355_IO, GPIO_MODE_OUTPUT);
    gpio_set_direction(SPI_CS_SCL3300_IO, GPIO_MODE_OUTPUT);

    gpio_set_level(SPI_CS_ADXL355_IO, 1);
    gpio_set_level(SPI_CS_SCL3300_IO, 1);

    vTaskDelay(pdMS_TO_TICKS(2));
}

/**
 * @brief Halt system on critical error
 */
static void halt_forever(void)
{
    ESP_LOGE(TAG, "*** HALT: Critical sensor init failed ***");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Processing task - reads from ring buffers and processes data
 * 
 * This task runs at low priority and can be interrupted at any time.
 * It reads raw data from ring buffers, converts to real units, and
 * eventually will publish via MQTT/Ethernet.
 * 
 * For now, it just logs the data periodically.
 */
static void sensor_processing_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Processing task started on core %d", xPortGetCoreID());
    
    // Raw sample structures
    adxl355_raw_sample_t adxl_raw;
    scl3300_raw_sample_t scl_raw;
    adt7420_raw_sample_t adt_raw;
    
    // Converted data (you'll use sensor driver functions to convert)
    // For now, we'll just display raw values
    
    uint32_t loop_count = 0;
    
    for (;;) {
        // Process ADXL355 data (highest rate - 2000 Hz)
        while (adxl355_read_sample(&adxl_raw)) {
            // TODO: Convert raw_x, raw_y, raw_z to g using ADXL355 conversion
            // For now, just count samples
            loop_count++;
        }
        
        // Process SCL3300 data (medium rate - 20 Hz)
        while (scl3300_read_sample(&scl_raw)) {
            // TODO: Convert raw_x, raw_y, raw_z to degrees using SCL3300 conversion
        }
        
        // Process ADT7420 data (low rate - 1 Hz)
        while (adt7420_read_sample(&adt_raw)) {
            // TODO: Convert raw_temp to Celsius using ADT7420 conversion
        }
        
        // Periodic status logging (every 2 seconds)
        if (loop_count % 1000 == 0) {
            ESP_LOGI(TAG, "Buffer status:");
            ESP_LOGI(TAG, "  ADXL355: %lu samples available, %lu total, %lu overflows",
                     (unsigned long)adxl355_samples_available(),
                     (unsigned long)adxl355_get_sample_count(),
                     (unsigned long)adxl355_get_overflow_count());
            ESP_LOGI(TAG, "  SCL3300: %lu samples available, %lu total, %lu overflows",
                     (unsigned long)scl3300_samples_available(),
                     (unsigned long)scl3300_get_sample_count(),
                     (unsigned long)scl3300_get_overflow_count());
            ESP_LOGI(TAG, "  ADT7420: %lu samples available, %lu total, %lu overflows",
                     (unsigned long)adt7420_samples_available(),
                     (unsigned long)adt7420_get_sample_count(),
                     (unsigned long)adt7420_get_overflow_count());
            
            // Check for buffer overflows
            if (adxl355_get_overflow_count() > 0 ||
                scl3300_get_overflow_count() > 0 ||
                adt7420_get_overflow_count() > 0) {
                ESP_LOGW(TAG, "WARNING: Buffer overflows detected! Processing too slow!");
            }
        }
        
        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, " ISR-Based Sensor Acquisition System  ");
    ESP_LOGI(TAG, "======================================");

    esp_err_t ret;
    bool temp_sensor_available = false;

    // ---- Step 1: Force SPI CS high ----
    ESP_LOGI(TAG, "Step 1: Forcing SPI CS lines high...");
    force_spi_cs_high_early();

    // ---- Step 2: Initialize buses ----
    ESP_LOGI(TAG, "Step 2: Initializing buses...");
    
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

    // ---- Step 3: Initialize sensors ----
    ESP_LOGI(TAG, "Step 3: Initializing sensors...");
    
    // ADT7420 (temperature - I2C)
    ret = adt7420_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADT7420 init failed: %s (continuing without temperature)", 
                 esp_err_to_name(ret));
        temp_sensor_available = false;
    } else {
        ESP_LOGI(TAG, "ADT7420 initialized ✓");
        temp_sensor_available = true;
    }

    // Ensure SCL3300 CS is high before ADXL355 init
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    // ADXL355 (accelerometer - SPI)
    ret = adxl355_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADXL355 init failed (CRITICAL): %s", esp_err_to_name(ret));
        halt_forever();
    }
    ESP_LOGI(TAG, "ADXL355 initialized ✓");

    // Ensure ADXL355 CS is high before SCL3300 init
    gpio_set_level(SPI_CS_ADXL355_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    // SCL3300 (inclinometer - SPI)
    ret = scl3300_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCL3300 init failed (CRITICAL): %s", esp_err_to_name(ret));
        halt_forever();
    }
    ESP_LOGI(TAG, "SCL3300 initialized ✓");

    ESP_LOGI(TAG, "All sensors initialized successfully ✅");

    // ---- Step 4: Initialize ISR-based acquisition ----
    ESP_LOGI(TAG, "Step 4: Initializing ISR-based acquisition...");
    
    ret = sensor_acquisition_init(temp_sensor_available);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor acquisition init failed: %s", esp_err_to_name(ret));
        halt_forever();
    }
    ESP_LOGI(TAG, "ISR acquisition initialized ✓");

    // Temporary verification
    ESP_LOGI(TAG, "Device handle check:");
    ESP_LOGI(TAG, "  adxl355_spi_handle = %p", adxl355_spi_handle);
    ESP_LOGI(TAG, "  scl3300_spi_handle = %p", scl3300_spi_handle);
    ESP_LOGI(TAG, "  adt7420_i2c_handle = %p", adt7420_i2c_handle);

if (adxl355_spi_handle == NULL) {
    ESP_LOGE(TAG, "ERROR: adxl355_spi_handle not exposed!");
}
if (scl3300_spi_handle == NULL) {
    ESP_LOGE(TAG, "ERROR: scl3300_spi_handle not exposed!");
}
if (adt7420_i2c_handle == NULL && temp_sensor_available) {
    ESP_LOGE(TAG, "ERROR: adt7420_i2c_handle not exposed!");
}

    // ---- Step 5: Create processing task ----
    ESP_LOGI(TAG, "Step 5: Creating processing task...");
    
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        sensor_processing_task,
        "proc_task",
        8192,           // Stack size
        NULL,
        2,              // Low priority
        NULL,
        tskNO_AFFINITY  // Can run on any core
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create processing task");
        halt_forever();
    }
    ESP_LOGI(TAG, "Processing task created ✓");

    // ---- Step 6: Start acquisition ----
    ESP_LOGI(TAG, "Step 6: Starting data acquisition...");
    
    ret = sensor_acquisition_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start acquisition: %s", esp_err_to_name(ret));
        halt_forever();
    }
    
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, " SYSTEM RUNNING ✅                    ");
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "ISR collecting data at:");
    ESP_LOGI(TAG, "  - ADXL355: 2000 Hz");
    ESP_LOGI(TAG, "  - SCL3300: 20 Hz");
    if (temp_sensor_available) {
        ESP_LOGI(TAG, "  - ADT7420: 1 Hz");
    }
    ESP_LOGI(TAG, "======================================");

    // Main task has nothing else to do - just monitor system health
    uint32_t last_tick = 0;
    
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // Every 5 seconds
        
        uint32_t current_tick = get_tick_count();
        uint32_t elapsed_ticks = current_tick - last_tick;
        float elapsed_sec = TICKS_TO_SEC(elapsed_ticks);
        
        ESP_LOGI(TAG, "System health: %.2f sec elapsed, tick=%lu", 
                 elapsed_sec, (unsigned long)current_tick);
        
        last_tick = current_tick;
        
        // Get overall statistics
        uint32_t acquired, dropped, max_time;
        sensor_acquisition_get_stats(&acquired, &dropped, &max_time);
        
        ESP_LOGI(TAG, "Total: %lu acquired, %lu dropped",
                 (unsigned long)acquired, (unsigned long)dropped);
        
        if (dropped > 0) {
            ESP_LOGW(TAG, "⚠️  DATA LOSS: %lu samples dropped!", (unsigned long)dropped);
        }
    }
}
