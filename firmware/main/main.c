/**
 * @file main.c
 * @brief Wind Turbine Structural Health Monitor - Timing Critical Version
 *
 * This implementation prioritizes deterministic timing for sensor acquisition:
 *
 * Architecture:
 * - Single hardware timer at base sample rate
 * - Single high-priority acquisition task (pinned to core)
 * - Single low-priority processing task (can be preempted)
 * - Lock-free queue between acquisition and processing
 *
 * Key design decisions:
 * - NO logging in acquisition path
 * - NO floating point formatting in acquisition path  
 * - NO blocking operations in acquisition path
 * - All sensors read sequentially (no bus contention)
 * - Statistics tracking for jitter monitoring
 *
 * Reference:
 * - https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gptimer.html
 * - https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos.html
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Bus drivers
#include "i2c_bus.h"
#include "spi_bus.h"

// Sensor drivers
#include "adt7420.h"
#include "adxl355.h"
#include "scl3300.h"

// Acquisition system
#include "sensor_task.h"

// Ethernet (NEW)
#include "ethernet.h"

static const char *TAG = "main";

// Ethernet timeout (NEW)
#define ETH_IP_TIMEOUT_MS   30000   // 30 seconds to get IP


/*******************************************************************************
 * Statistics Monitor Task
 *
 * Periodically prints acquisition statistics to monitor system health.
 * Runs at very low priority.
 ******************************************************************************/

#define STATS_TASK_PRIORITY     1
#define STATS_TASK_STACK_SIZE   4096
#define STATS_INTERVAL_MS       10000   // Print stats every 10 seconds

static void stats_monitor_task(void *pvParameters)
{
    uint32_t samples_acquired, samples_dropped, max_acq_time;
    uint32_t prev_samples = 0;
    esp_netif_ip_info_t ip_info;  // NEW
    
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_MS));
        
        sensor_acquisition_get_stats(&samples_acquired, &samples_dropped, &max_acq_time);
        
        uint32_t samples_this_interval = samples_acquired - prev_samples;
        prev_samples = samples_acquired;
        
        ESP_LOGI("STATS", "==== Acquisition Statistics ====");
        ESP_LOGI("STATS", "  Samples acquired: %lu total, %lu in last %d sec",
                 (unsigned long)samples_acquired,
                 (unsigned long)samples_this_interval,
                 STATS_INTERVAL_MS / 1000);
        ESP_LOGI("STATS", "  Samples dropped: %lu (queue full)",
                 (unsigned long)samples_dropped);
        ESP_LOGI("STATS", "  Max acquisition time: %lu us (jitter indicator)",
                 (unsigned long)max_acq_time);
        
        // Network status (NEW)
        if (ethernet_is_connected()) {
            ethernet_get_ip_info(&ip_info);
            ESP_LOGI("STATS", "  Network: Connected (" IPSTR ")", IP2STR(&ip_info.ip));
        } else {
            ESP_LOGW("STATS", "  Network: Disconnected");
        }
        
        ESP_LOGI("STATS", "  Free heap: %lu bytes",
                 (unsigned long)esp_get_free_heap_size());
        ESP_LOGI("STATS", "================================");
    }
}


/*******************************************************************************
 * Initialization Functions
 ******************************************************************************/

static void print_banner(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Wind Turbine Structural Health Monitor");
    ESP_LOGI(TAG, "  TIMING-CRITICAL ACQUISITION VERSION");
    ESP_LOGI(TAG, "          Capstone Project");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Architecture:");
    ESP_LOGI(TAG, "  - Single HW timer at base rate");
    ESP_LOGI(TAG, "  - High-priority acquisition task (no logging)");
    ESP_LOGI(TAG, "  - Low-priority processing task (handles logging)");
    ESP_LOGI(TAG, "  - Lock-free queue between tasks");
    ESP_LOGI(TAG, "  - Ethernet connectivity (ESP32-POE-ISO)");  // NEW
    ESP_LOGI(TAG, "");
}

// NEW: Ethernet initialization function
static esp_err_t init_network(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet...");
    
    if (ethernet_init() != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet init failed");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Waiting for IP address (timeout: %d sec)...", ETH_IP_TIMEOUT_MS / 1000);
    esp_err_t ret = ethernet_wait_for_ip(ETH_IP_TIMEOUT_MS);
    
    if (ret == ESP_OK) {
        esp_netif_ip_info_t ip_info;
        ethernet_get_ip_info(&ip_info);
        ESP_LOGI(TAG, "Network ready: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGW(TAG, "No IP address yet - continuing anyway");
        ESP_LOGW(TAG, "Check Ethernet cable connection");
    }
    
    return ESP_OK;  // Don't fail - network might come up later
}

static esp_err_t init_buses(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing I2C bus...");
    ret = i2c_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initializing SPI bus...");
    ret = spi_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Communication buses initialized");
    return ESP_OK;
}

static esp_err_t init_sensors(bool *temp_available)
{
    esp_err_t ret;
    *temp_available = false;

    // Temperature sensor (non-critical)
    ESP_LOGI(TAG, "Initializing ADT7420 temperature sensor...");
    ret = adt7420_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADT7420 init failed - continuing without temperature");
    } else {
        ESP_LOGI(TAG, "ADT7420 initialized");
        *temp_available = true;
    }

    // Accelerometer (critical)
    ESP_LOGI(TAG, "Initializing ADXL355 accelerometer...");
    ret = adxl355_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADXL355 init failed - CRITICAL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ADXL355 initialized");

    // Inclinometer (critical)
    ESP_LOGI(TAG, "Initializing SCL3300 inclinometer...");
    ret = scl3300_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCL3300 init failed - CRITICAL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SCL3300 initialized");

    return ESP_OK;
}

static void print_config(bool temp_available)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Acquisition Configuration");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Base timer period: %d us (%d Hz)",
             BASE_SAMPLE_PERIOD_US, (int)(1000000 / BASE_SAMPLE_PERIOD_US));
    ESP_LOGI(TAG, "  Accelerometer: %d Hz (decimation=%d)",
             (int)(1000000 / BASE_SAMPLE_PERIOD_US / ACCEL_DECIMATION), ACCEL_DECIMATION);
    ESP_LOGI(TAG, "  Inclinometer:  %d Hz (decimation=%d)",
             (int)(1000000 / BASE_SAMPLE_PERIOD_US / ANGLE_DECIMATION), ANGLE_DECIMATION);
    if (temp_available) {
        ESP_LOGI(TAG, "  Temperature:   %d Hz (decimation=%d)",
                 (int)(1000000 / BASE_SAMPLE_PERIOD_US / TEMP_DECIMATION), TEMP_DECIMATION);
    } else {
        ESP_LOGW(TAG, "  Temperature:   OFFLINE");
    }
    ESP_LOGI(TAG, "  Queue size: %d samples", SENSOR_QUEUE_SIZE);
    ESP_LOGI(TAG, "  Acquisition task: priority=%d, core=%d",
             ACQUISITION_TASK_PRIORITY, ACQUISITION_TASK_CORE);
    ESP_LOGI(TAG, "  Processing task:  priority=%d",
             PROCESSING_TASK_PRIORITY);
    
    // Network status (NEW)
    if (ethernet_is_connected()) {
        ESP_LOGI(TAG, "  Network: CONNECTED");
    } else {
        ESP_LOGW(TAG, "  Network: WAITING FOR CONNECTION");
    }
    
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "");
}


/*******************************************************************************
 * Main Application Entry Point
 ******************************************************************************/

void app_main(void)
{
    bool temp_sensor_available = false;

    print_banner();

    // Initialize Ethernet FIRST (NEW - takes time to get IP)
    ESP_LOGI(TAG, "--- Network Initialization ---");
    init_network();
    ESP_LOGI(TAG, "");

    /*
	// Initialize buses
    ESP_LOGI(TAG, "--- Bus Initialization ---");
    if (init_buses() != ESP_OK) {
        ESP_LOGE(TAG, "*** SYSTEM HALTED: Bus init failed ***");
        return;
    }

    // Initialize sensors
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "--- Sensor Initialization ---");
    if (init_sensors(&temp_sensor_available) != ESP_OK) {
        ESP_LOGE(TAG, "*** SYSTEM HALTED: Critical sensor init failed ***");
        return;
    }

    // Print configuration
    print_config(temp_sensor_available);

    // Initialize acquisition system (creates timer, queue, tasks)
    ESP_LOGI(TAG, "--- Acquisition System Initialization ---");
    if (sensor_acquisition_init(temp_sensor_available) != ESP_OK) {
        ESP_LOGE(TAG, "*** SYSTEM HALTED: Acquisition init failed ***");
        return;
    }

    // Create statistics monitor task
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "--- Creating Statistics Monitor ---");
    xTaskCreate(
        stats_monitor_task,
        "stats_task",
        STATS_TASK_STACK_SIZE,
        NULL,
        STATS_TASK_PRIORITY,
        NULL
    );
    ESP_LOGI(TAG, "Statistics monitor created (interval=%d ms)", STATS_INTERVAL_MS);

    // Start acquisition
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "--- Starting Acquisition ---");
    if (sensor_acquisition_start() != ESP_OK) {
        ESP_LOGE(TAG, "*** SYSTEM HALTED: Failed to start acquisition ***");
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  SYSTEM RUNNING");
    ESP_LOGI(TAG, "  Sensor data will appear below");
    ESP_LOGI(TAG, "  Statistics printed every %d seconds", STATS_INTERVAL_MS / 1000);
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "");
	
	*/
	
	while (1) {
	        if (ethernet_is_connected()) {
	            ESP_LOGI(TAG, "Ethernet: CONNECTED");
	        } else {
	            ESP_LOGW(TAG, "Ethernet: Waiting...");
	        }
	        vTaskDelay(pdMS_TO_TICKS(5000));
	    }

    // app_main() returns - tasks continue running
}