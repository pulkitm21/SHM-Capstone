//
///**
// * @file main.c
// * @brief Wind Turbine Structural Health Monitor
// *
// * Architecture:
// * - DAQ Task (high priority): Reads sensors, pushes raw data to queue
// * - MQTT Task (low priority): Pulls from queue, converts units, publishes JSON
// * - Ethernet connectivity for MQTT communication
// *
// * Data Flow:
// * Sensors → DAQ Task → Queue → MQTT Task → JSON → Broker → Raspberry Pi
// */
//
//
//#include <stdio.h>
//#include "freertos/FreeRTOS.h"
//#include "freertos/task.h"
//#include "esp_log.h"
//
//// Bus drivers
//#include "i2c_bus.h"
//#include "spi_bus.h"
//
//// OLD: Sensor drivers (not needed - teammate handles sensors in DAQ task)
//// #include "adt7420.h"
//// #include "adxl355.h"
//// #include "scl3300.h"
//
//// OLD: Acquisition system (replaced by DAQ task + MQTT task)
//// #include "sensor_task.h"
//
//// Network & MQTT (NEW)
//#include "ethernet.h"
//#include "mqtt.h"
//#include "mqtt_task.h"
//
//// Shared data types (NEW)
//#include "sensor_types.h"
//
//static const char *TAG = "main";
//
//// Timeouts
//#define ETH_IP_TIMEOUT_MS       30000   // 30 seconds to get IP
//#define MQTT_CONNECT_TIMEOUT_MS 30000   // 30 seconds to connect to broker (NEW)
//
//
///*******************************************************************************
// * Statistics Monitor Task
// *
// * Periodically prints system statistics.
// * Runs at very low priority.
// ******************************************************************************/
//
//#define STATS_TASK_PRIORITY     1
//#define STATS_TASK_STACK_SIZE   4096
//#define STATS_INTERVAL_MS       10000   // Print stats every 10 seconds
//
//static void stats_monitor_task(void *pvParameters)
//{
//    // OLD: Used sensor_acquisition_get_stats
//    // uint32_t samples_acquired, samples_dropped, max_acq_time;
//    // uint32_t prev_samples = 0;
//
//    // NEW: Use mqtt_task_get_stats
//    uint32_t samples_published, packets_sent, samples_dropped;
//    esp_netif_ip_info_t ip_info;
//
//    for (;;) {
//        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_MS));
//
//        // OLD: Get stats from old acquisition system
//        // sensor_acquisition_get_stats(&samples_acquired, &samples_dropped, &max_acq_time);
//        // uint32_t samples_this_interval = samples_acquired - prev_samples;
//        // prev_samples = samples_acquired;
//
//        // NEW: Get MQTT task statistics
//        mqtt_task_get_stats(&samples_published, &packets_sent, &samples_dropped);
//
//        ESP_LOGI("STATS", "==== System Statistics ====");
//
//        // OLD stats format
//        // ESP_LOGI("STATS", "  Samples acquired: %lu total, %lu in last %d sec",
//        //          (unsigned long)samples_acquired,
//        //          (unsigned long)samples_this_interval,
//        //          STATS_INTERVAL_MS / 1000);
//        // ESP_LOGI("STATS", "  Samples dropped: %lu (queue full)",
//        //          (unsigned long)samples_dropped);
//        // ESP_LOGI("STATS", "  Max acquisition time: %lu us (jitter indicator)",
//        //          (unsigned long)max_acq_time);
//
//        // NEW stats format
//        ESP_LOGI("STATS", "  Samples published: %lu", (unsigned long)samples_published);
//        ESP_LOGI("STATS", "  MQTT packets sent: %lu", (unsigned long)packets_sent);
//        ESP_LOGI("STATS", "  Samples dropped:   %lu", (unsigned long)samples_dropped);
//
//        // Network status
//        if (ethernet_is_connected()) {
//            ethernet_get_ip_info(&ip_info);
//            ESP_LOGI("STATS", "  Network: Connected (" IPSTR ")", IP2STR(&ip_info.ip));
//        } else {
//            ESP_LOGW("STATS", "  Network: Disconnected");
//        }
//
//        // NEW: MQTT status
//        if (mqtt_is_connected()) {
//            ESP_LOGI("STATS", "  MQTT: Connected");
//        } else {
//            ESP_LOGW("STATS", "  MQTT: Disconnected");
//        }
//
//        ESP_LOGI("STATS", "  Free heap: %lu bytes",
//                 (unsigned long)esp_get_free_heap_size());
//        ESP_LOGI("STATS", "===========================");
//    }
//}
//
//
///*******************************************************************************
// * Initialization Functions
// ******************************************************************************/
//
//static void print_banner(void)
//{
//    ESP_LOGI(TAG, "");
//    ESP_LOGI(TAG, "================================================");
//    ESP_LOGI(TAG, "  Wind Turbine Structural Health Monitor");
//    // OLD banner text
//    // ESP_LOGI(TAG, "  TIMING-CRITICAL ACQUISITION VERSION");
//    ESP_LOGI(TAG, "          Capstone Project");
//    ESP_LOGI(TAG, "================================================");
//    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
//    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
//    ESP_LOGI(TAG, "");
//    ESP_LOGI(TAG, "Architecture:");
//    // OLD architecture description
//    // ESP_LOGI(TAG, "  - Single HW timer at base rate");
//    // ESP_LOGI(TAG, "  - High-priority acquisition task (no logging)");
//    // ESP_LOGI(TAG, "  - Low-priority processing task (handles logging)");
//    // ESP_LOGI(TAG, "  - Lock-free queue between tasks");
//    // ESP_LOGI(TAG, "  - Ethernet connectivity (ESP32-POE-ISO)");
//    // NEW architecture description
//    ESP_LOGI(TAG, "  - DAQ Task: Sensor reading (high priority)");
//    ESP_LOGI(TAG, "  - MQTT Task: Unit conversion + publishing (low priority)");
//    ESP_LOGI(TAG, "  - FreeRTOS Queue between tasks");
//    ESP_LOGI(TAG, "  - Ethernet → MQTT → Raspberry Pi");
//    ESP_LOGI(TAG, "");
//}
//
//static esp_err_t init_network(void)
//{
//    ESP_LOGI(TAG, "Initializing Ethernet...");
//
//    if (ethernet_init() != ESP_OK) {
//        ESP_LOGE(TAG, "Ethernet init failed");
//        return ESP_FAIL;
//    }
//
//    ESP_LOGI(TAG, "Waiting for IP address (timeout: %d sec)...", ETH_IP_TIMEOUT_MS / 1000);
//    esp_err_t ret = ethernet_wait_for_ip(ETH_IP_TIMEOUT_MS);
//
//    if (ret == ESP_OK) {
//        esp_netif_ip_info_t ip_info;
//        ethernet_get_ip_info(&ip_info);
//        ESP_LOGI(TAG, "Network ready: " IPSTR, IP2STR(&ip_info.ip));
//    } else {
//        ESP_LOGW(TAG, "No IP address yet - continuing anyway");
//        ESP_LOGW(TAG, "Check Ethernet cable connection");
//    }
//
//    return ESP_OK;  // Don't fail - network might come up later
//}
//
//// NEW: MQTT initialization function
//static esp_err_t init_mqtt(void)
//{
//    ESP_LOGI(TAG, "Initializing MQTT client...");
//
//    if (mqtt_init() != ESP_OK) {
//        ESP_LOGE(TAG, "MQTT init failed");
//        return ESP_FAIL;
//    }
//
//    ESP_LOGI(TAG, "Waiting for MQTT connection (timeout: %d sec)...", MQTT_CONNECT_TIMEOUT_MS / 1000);
//    esp_err_t ret = mqtt_wait_for_connection(MQTT_CONNECT_TIMEOUT_MS);
//
//    if (ret == ESP_OK) {
//        ESP_LOGI(TAG, "MQTT connected!");
//        mqtt_publish_status("Wind Turbine Monitor Online");
//    } else {
//        ESP_LOGW(TAG, "MQTT connection timeout - will retry in background");
//    }
//
//    return ESP_OK;
//}
//
//static esp_err_t init_buses(void)
//{
//    esp_err_t ret;
//
//    ESP_LOGI(TAG, "Initializing I2C bus...");
//    ret = i2c_bus_init();
//    if (ret != ESP_OK) {
//        ESP_LOGE(TAG, "I2C bus init failed");
//        return ESP_FAIL;
//    }
//
//    ESP_LOGI(TAG, "Initializing SPI bus...");
//    ret = spi_bus_init();
//    if (ret != ESP_OK) {
//        ESP_LOGE(TAG, "SPI bus init failed");
//        return ESP_FAIL;
//    }
//
//    ESP_LOGI(TAG, "Communication buses initialized");
//    return ESP_OK;
//}
//
//// OLD: Sensor initialization (not needed - teammate handles in DAQ task)
///*
//static esp_err_t init_sensors(bool *temp_available)
//{
//    esp_err_t ret;
//    *temp_available = false;
//
//    // Temperature sensor (non-critical)
//    ESP_LOGI(TAG, "Initializing ADT7420 temperature sensor...");
//    ret = adt7420_init();
//    if (ret != ESP_OK) {
//        ESP_LOGW(TAG, "ADT7420 init failed - continuing without temperature");
//    } else {
//        ESP_LOGI(TAG, "ADT7420 initialized");
//        *temp_available = true;
//    }
//
//    // Accelerometer (critical)
//    ESP_LOGI(TAG, "Initializing ADXL355 accelerometer...");
//    ret = adxl355_init();
//    if (ret != ESP_OK) {
//        ESP_LOGE(TAG, "ADXL355 init failed - CRITICAL");
//        return ESP_FAIL;
//    }
//    ESP_LOGI(TAG, "ADXL355 initialized");
//
//    // Inclinometer (critical)
//    ESP_LOGI(TAG, "Initializing SCL3300 inclinometer...");
//    ret = scl3300_init();
//    if (ret != ESP_OK) {
//        ESP_LOGE(TAG, "SCL3300 init failed - CRITICAL");
//        return ESP_FAIL;
//    }
//    ESP_LOGI(TAG, "SCL3300 initialized");
//
//    return ESP_OK;
//}
//*/
//
//// OLD: Print configuration (referenced old sensor_task.h defines)
///*
//static void print_config(bool temp_available)
//{
//    ESP_LOGI(TAG, "");
//    ESP_LOGI(TAG, "================================================");
//    ESP_LOGI(TAG, "  Acquisition Configuration");
//    ESP_LOGI(TAG, "================================================");
//    ESP_LOGI(TAG, "  Base timer period: %d us (%d Hz)",
//             BASE_SAMPLE_PERIOD_US, (int)(1000000 / BASE_SAMPLE_PERIOD_US));
//    ESP_LOGI(TAG, "  Accelerometer: %d Hz (decimation=%d)",
//             (int)(1000000 / BASE_SAMPLE_PERIOD_US / ACCEL_DECIMATION), ACCEL_DECIMATION);
//    ESP_LOGI(TAG, "  Inclinometer:  %d Hz (decimation=%d)",
//             (int)(1000000 / BASE_SAMPLE_PERIOD_US / ANGLE_DECIMATION), ANGLE_DECIMATION);
//    if (temp_available) {
//        ESP_LOGI(TAG, "  Temperature:   %d Hz (decimation=%d)",
//                 (int)(1000000 / BASE_SAMPLE_PERIOD_US / TEMP_DECIMATION), TEMP_DECIMATION);
//    } else {
//        ESP_LOGW(TAG, "  Temperature:   OFFLINE");
//    }
//    ESP_LOGI(TAG, "  Queue size: %d samples", SENSOR_QUEUE_SIZE);
//    ESP_LOGI(TAG, "  Acquisition task: priority=%d, core=%d",
//             ACQUISITION_TASK_PRIORITY, ACQUISITION_TASK_CORE);
//    ESP_LOGI(TAG, "  Processing task:  priority=%d",
//             PROCESSING_TASK_PRIORITY);
//
//    // Network status
//    if (ethernet_is_connected()) {
//        ESP_LOGI(TAG, "  Network: CONNECTED");
//    } else {
//        ESP_LOGW(TAG, "  Network: WAITING FOR CONNECTION");
//    }
//
//    ESP_LOGI(TAG, "================================================");
//    ESP_LOGI(TAG, "");
//}
//*/
//
//
///*******************************************************************************
// * Main Application Entry Point
// ******************************************************************************/
//
//void app_main(void)
//{
//    // OLD: Used for sensor init
//    // bool temp_sensor_available = false;
//
//    print_banner();
//
//    // =========================================
//    // 1. Initialize Network (Ethernet)
//    // =========================================
//    ESP_LOGI(TAG, "--- Network Initialization ---");
//    if (init_network() != ESP_OK) {
//        ESP_LOGE(TAG, "*** Network init failed - continuing anyway ***");
//    }
//    ESP_LOGI(TAG, "");
//
//    // =========================================
//    // 2. Initialize MQTT Client (NEW)
//    // =========================================
//    ESP_LOGI(TAG, "--- MQTT Initialization ---");
//    if (init_mqtt() != ESP_OK) {
//        ESP_LOGE(TAG, "*** MQTT init failed - continuing anyway ***");
//    }
//    ESP_LOGI(TAG, "");
//
//    // =========================================
//    // 3. Initialize Communication Buses
//    // =========================================
//    ESP_LOGI(TAG, "--- Bus Initialization ---");
//    if (init_buses() != ESP_OK) {
//        ESP_LOGE(TAG, "*** SYSTEM HALTED: Bus init failed ***");
//        return;
//    }
//    ESP_LOGI(TAG, "");
//
//    // OLD: Initialize sensors (not needed - teammate handles in DAQ task)
//    /*
//    ESP_LOGI(TAG, "--- Sensor Initialization ---");
//    if (init_sensors(&temp_sensor_available) != ESP_OK) {
//        ESP_LOGE(TAG, "*** SYSTEM HALTED: Critical sensor init failed ***");
//        return;
//    }
//    */
//
//    // OLD: Print configuration
//    // print_config(temp_sensor_available);
//
//    // OLD: Initialize old acquisition system
//    /*
//    ESP_LOGI(TAG, "--- Acquisition System Initialization ---");
//    if (sensor_acquisition_init(temp_sensor_available) != ESP_OK) {
//        ESP_LOGE(TAG, "*** SYSTEM HALTED: Acquisition init failed ***");
//        return;
//    }
//    */
//
//    // =========================================
//    // 4. Initialize MQTT Task (NEW - creates queue)
//    // =========================================
//    ESP_LOGI(TAG, "--- MQTT Task Initialization ---");
//    if (mqtt_task_init() != ESP_OK) {
//        ESP_LOGE(TAG, "*** SYSTEM HALTED: MQTT task init failed ***");
//        return;
//    }
//    ESP_LOGI(TAG, "");
//
//    // =========================================
//    // 5. Initialize DAQ Task (NEW - teammate adds this)
//    // =========================================
//    ESP_LOGI(TAG, "--- DAQ Task Initialization ---");
//    // TODO: Your teammate adds daq_task_init() here
//    // This task will:
//    //   - Get queue handle: mqtt_task_get_queue()
//    //   - Read sensors at 2000 Hz
//    //   - Push raw_sample_t to queue
//    ESP_LOGW(TAG, "DAQ task not implemented yet - waiting for teammate");
//    ESP_LOGI(TAG, "");
//
//    // =========================================
//    // 6. Create Statistics Monitor Task
//    // =========================================
//    ESP_LOGI(TAG, "--- Creating Statistics Monitor ---");
//    xTaskCreate(
//        stats_monitor_task,
//        "stats_task",
//        STATS_TASK_STACK_SIZE,
//        NULL,
//        STATS_TASK_PRIORITY,
//        NULL
//    );
//    ESP_LOGI(TAG, "Statistics monitor created (interval=%d ms)", STATS_INTERVAL_MS);
//
//    // OLD: Start old acquisition system
//    /*
//    ESP_LOGI(TAG, "--- Starting Acquisition ---");
//    if (sensor_acquisition_start() != ESP_OK) {
//        ESP_LOGE(TAG, "*** SYSTEM HALTED: Failed to start acquisition ***");
//        return;
//    }
//    */
//
//    // =========================================
//    // System Running
//    // =========================================
//    ESP_LOGI(TAG, "");
//    ESP_LOGI(TAG, "================================================");
//    ESP_LOGI(TAG, "  SYSTEM RUNNING");
//    ESP_LOGI(TAG, "  Waiting for DAQ task to push sensor data...");
//    ESP_LOGI(TAG, "  Statistics printed every %d seconds", STATS_INTERVAL_MS / 1000);
//    ESP_LOGI(TAG, "================================================");
//    ESP_LOGI(TAG, "");
//
//    // OLD: Test loop (was just for testing ethernet)
//    /*
//    while (1) {
//        if (ethernet_is_connected()) {
//            ESP_LOGI(TAG, "Ethernet: CONNECTED");
//        } else {
//            ESP_LOGW(TAG, "Ethernet: Waiting...");
//        }
//        vTaskDelay(pdMS_TO_TICKS(5000));
//    }
//    */
//
//    // app_main() returns - tasks continue running
//}

















/**
 * @file main.c
 * @brief Wind Turbine Structural Health Monitor - TESTING VERSION
 *
 * This version uses a FAKE DAQ task to test MQTT communication.
 * Replace fake_daq_task with real daq_task when ready.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Bus drivers
#include "i2c_bus.h"
#include "spi_bus.h"

// Network & MQTT
#include "ethernet.h"
#include "mqtt.h"
#include "data_processing_and_mqtt_task.h"

// Shared data types
#include "sensor_types.h"

// TESTING: Fake DAQ task (DELETE when real DAQ is ready)
#include "fake_daq_task.h"

static const char *TAG = "main";

// Timeouts
#define ETH_IP_TIMEOUT_MS       30000   // 30 seconds to get IP
#define MQTT_CONNECT_TIMEOUT_MS 30000   // 30 seconds to connect to broker


/*******************************************************************************
 * Statistics Monitor Task
 ******************************************************************************/

#define STATS_TASK_PRIORITY     1
#define STATS_TASK_STACK_SIZE   4096
#define STATS_INTERVAL_MS       10000   // Print stats every 10 seconds

static void stats_monitor_task(void *pvParameters)
{
    uint32_t samples_published, packets_sent, samples_dropped;
    esp_netif_ip_info_t ip_info;
    
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_MS));
        
        mqtt_task_get_stats(&samples_published, &packets_sent, &samples_dropped);

        ESP_LOGI("STATS", "==== System Statistics ====");
        ESP_LOGI("STATS", "  Samples published: %lu", (unsigned long)samples_published);
        ESP_LOGI("STATS", "  MQTT packets sent: %lu", (unsigned long)packets_sent);
        ESP_LOGI("STATS", "  Samples dropped:   %lu", (unsigned long)samples_dropped);
        
        // Network status
        if (ethernet_is_connected()) {
            ethernet_get_ip_info(&ip_info);
            ESP_LOGI("STATS", "  Network: Connected (" IPSTR ")", IP2STR(&ip_info.ip));
        } else {
            ESP_LOGW("STATS", "  Network: Disconnected");
        }
        
        // MQTT status
        if (mqtt_is_connected()) {
            ESP_LOGI("STATS", "  MQTT: Connected");
        } else {
            ESP_LOGW("STATS", "  MQTT: Disconnected");
        }

        ESP_LOGI("STATS", "  Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
        ESP_LOGI("STATS", "===========================");
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
    ESP_LOGI(TAG, "  *** TESTING MODE - FAKE SENSOR DATA ***");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "");
}

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
    }
    
    return ESP_OK;
}

static esp_err_t init_mqtt(void)
{
    ESP_LOGI(TAG, "Initializing MQTT client...");

    if (mqtt_init() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Waiting for MQTT connection (timeout: %d sec)...", MQTT_CONNECT_TIMEOUT_MS / 1000);
    esp_err_t ret = mqtt_wait_for_connection(MQTT_CONNECT_TIMEOUT_MS);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT connected!");
        mqtt_publish_status("Wind Turbine Monitor Online - TEST MODE");
    } else {
        ESP_LOGW(TAG, "MQTT connection timeout - will retry in background");
    }

    return ESP_OK;
}

// Buses not needed for fake DAQ, but keep for when real DAQ is ready
/*
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
*/


/*******************************************************************************
 * Main Application Entry Point
 ******************************************************************************/

void app_main(void)
{
    print_banner();

    // =========================================
    // 1. Initialize Network (Ethernet)
    // =========================================
    ESP_LOGI(TAG, "--- Network Initialization ---");
    if (init_network() != ESP_OK) {
        ESP_LOGE(TAG, "*** Network init failed - continuing anyway ***");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 2. Initialize MQTT Client
    // =========================================
    ESP_LOGI(TAG, "--- MQTT Initialization ---");
    if (init_mqtt() != ESP_OK) {
        ESP_LOGE(TAG, "*** MQTT init failed - continuing anyway ***");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 3. Buses (skip for testing - fake DAQ doesn't need them)
    // =========================================
    // Uncomment when using real DAQ task:
    // ESP_LOGI(TAG, "--- Bus Initialization ---");
    // if (init_buses() != ESP_OK) {
    //     ESP_LOGE(TAG, "*** SYSTEM HALTED: Bus init failed ***");
    //     return;
    // }

    // =========================================
    // 4. Initialize MQTT Task (creates queue)
    // =========================================
    ESP_LOGI(TAG, "--- MQTT Task Initialization ---");
    if (mqtt_task_init() != ESP_OK) {
        ESP_LOGE(TAG, "*** SYSTEM HALTED: MQTT task init failed ***");
        return;
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 5. Initialize DAQ Task
    // =========================================
    ESP_LOGI(TAG, "--- DAQ Task Initialization ---");

    // TESTING: Use fake DAQ task
    if (fake_daq_task_init() != ESP_OK) {
        ESP_LOGE(TAG, "*** SYSTEM HALTED: Fake DAQ task init failed ***");
        return;
    }

    // PRODUCTION: Use real DAQ task (uncomment when ready)
    // if (daq_task_init() != ESP_OK) {
    //     ESP_LOGE(TAG, "*** SYSTEM HALTED: DAQ task init failed ***");
    //     return;
    // }

    ESP_LOGI(TAG, "");

    // =========================================
    // 6. Create Statistics Monitor Task
    // =========================================
    ESP_LOGI(TAG, "--- Creating Statistics Monitor ---");
    xTaskCreate(
        stats_monitor_task,
        "stats_task",
        STATS_TASK_STACK_SIZE,
        NULL,
        STATS_TASK_PRIORITY,
        NULL
    );
    ESP_LOGI(TAG, "");

    // =========================================
    // System Running
    // =========================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  SYSTEM RUNNING - TEST MODE");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Fake sensor data being generated");
    ESP_LOGI(TAG, "  Check Raspberry Pi for MQTT messages");
    ESP_LOGI(TAG, "  Topic: wind_turbine/data");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "");

    // app_main() returns - tasks continue running
}
