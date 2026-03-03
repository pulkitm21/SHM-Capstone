/**
 * @file main.c
 * @brief Wind Turbine Structural Health Monitor
 *
 * Merged Architecture:
 *  - Ethernet + MQTT publishing (data_processing_and_mqtt_task)
 *  - ISR-based sensor acquisition (from isr-daq branch)
 *  - Statistics monitor
 *
 * Error Handling:
 *  - Critical failures trigger automatic reboot after delay
 *  - Maximum reboot attempts tracked in memory
 *  - Prevents infinite reboot loops
 *
 * Data Flow:
 *  ISR (8000 Hz) → Ring Buffers → Data Processing Task → MQTT → Raspberry Pi
 *
 * FAULT LOGGING (boot-time):
 *  At the top of app_main(), esp_reset_reason() is checked and fault codes
 *  are recorded before any other initialization. These codes will be attached
 *  to the first outgoing MQTT data packet so the Raspberry Pi immediately
 *  knows why the device rebooted.
 *
 *  FAULT_WATCHDOG_RESET (11): previous reset was caused by watchdog timeout
 *  FAULT_POWER_LOSS (12):     previous reset was a power-on (power was lost)
 *  FAULT_POWER_RESTORED (13): logged alongside FAULT_POWER_LOSS at boot
 *  FAULT_REBOOT_ATTEMPT (10): logged just before esp_restart() is called
 *  FAULT_ADXL355_INIT_FAIL (14): adxl355_init() returned an error
 *  FAULT_SCL3300_INIT_FAIL (15): scl3300_init() returned an error
 *  FAULT_ADT7420_INIT_FAIL (16): adt7420_init() returned an error
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_idf_version.h"

#include "driver/gpio.h"

// Buses
#include "i2c_bus.h"
#include "spi_bus.h"

// Sensors
#include "adt7420.h"
#include "adxl355.h"
#include "scl3300.h"
#include "sensor_task.h"

// Network + MQTT
#include "ethernet.h"
#include "mqtt.h"
#include "data_processing_and_mqtt_task.h"

// Fault logging
#include "fault_log.h"

static const char *TAG = "main";

// Timeouts
#define ETH_IP_TIMEOUT_MS       30000
#define MQTT_CONNECT_TIMEOUT_MS 30000

// Reboot configuration
#define REBOOT_DELAY_MS         5000
#define MAX_REBOOT_ATTEMPTS     5

// Stats
#define STATS_TASK_PRIORITY     1
#define STATS_TASK_STACK_SIZE   4096
#define STATS_INTERVAL_MS       10000

// RTC memory survives reboots (not power cycles)
RTC_NOINIT_ATTR static uint32_t s_reboot_count;
RTC_NOINIT_ATTR static uint32_t s_reboot_magic;

#define REBOOT_MAGIC_VALUE      0xDEADBEEF

/* -------------------------------------------------------------------------- */
/* Utility Functions                                                          */
/* -------------------------------------------------------------------------- */

static void force_spi_cs_high_early(void)
{
#ifdef SPI_CS_ADXL355_IO
    gpio_set_direction(SPI_CS_ADXL355_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(SPI_CS_ADXL355_IO, 1);
#endif
#ifdef SPI_CS_SCL3300_IO
    gpio_set_direction(SPI_CS_SCL3300_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
#endif
    vTaskDelay(pdMS_TO_TICKS(2));
}

/**
 * @brief Check reset reason and record appropriate fault codes.
 *
 * Called at the very top of app_main() before any other initialization.
 * Fault codes recorded here will be attached to the first outgoing MQTT
 * packet once the network stack is up.
 *
 * Reset reason mapping:
 *   ESP_RST_POWERON  → power was lost and restored: log FAULT_POWER_LOSS + FAULT_POWER_RESTORED
 *   ESP_RST_WDT      → watchdog fired:              log FAULT_WATCHDOG_RESET
 *   ESP_RST_SW       → esp_restart() was called:    log FAULT_REBOOT_ATTEMPT
 *   anything else    → no fault logged
 */
static void check_reset_reason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    switch (reason) {
        case ESP_RST_POWERON:
            ESP_LOGI(TAG, "Reset reason: POWER-ON (power was lost and restored)");
            fault_log_record(FAULT_POWER_LOSS);
            fault_log_record(FAULT_POWER_RESTORED);
            break;

        case ESP_RST_WDT:
            ESP_LOGW(TAG, "Reset reason: WATCHDOG TIMEOUT");
            fault_log_record(FAULT_WATCHDOG_RESET);
            break;

        case ESP_RST_SW:
            ESP_LOGW(TAG, "Reset reason: SOFTWARE RESTART (esp_restart called)");
            fault_log_record(FAULT_REBOOT_ATTEMPT);
            break;

        case ESP_RST_DEEPSLEEP:
            ESP_LOGI(TAG, "Reset reason: deep sleep wakeup");
            break;

        case ESP_RST_BROWNOUT:
            /* Brownout can indicate power instability — treat same as power loss */
            ESP_LOGW(TAG, "Reset reason: BROWNOUT");
            fault_log_record(FAULT_POWER_LOSS);
            fault_log_record(FAULT_POWER_RESTORED);
            break;

        default:
            ESP_LOGI(TAG, "Reset reason: %d (no fault logged)", (int)reason);
            break;
    }
}

static void init_reboot_counter(void)
{
    if (s_reboot_magic != REBOOT_MAGIC_VALUE) {
        s_reboot_count = 0;
        s_reboot_magic = REBOOT_MAGIC_VALUE;
        ESP_LOGI(TAG, "Fresh boot detected - reboot counter reset");
    } else {
        ESP_LOGW(TAG, "Reboot detected - attempt %lu of %d",
                 (unsigned long)(s_reboot_count + 1), MAX_REBOOT_ATTEMPTS);
    }
}

static void clear_reboot_counter(void)
{
    s_reboot_count = 0;
    ESP_LOGI(TAG, "Initialization successful - reboot counter cleared");
}

/**
 * @brief Handle critical failure: record reboot fault, then reboot or halt.
 */
static void handle_critical_failure(const char *reason)
{
    ESP_LOGE(TAG, "*** CRITICAL FAILURE: %s ***", reason);

    s_reboot_count++;

    if (s_reboot_count >= MAX_REBOOT_ATTEMPTS) {
        ESP_LOGE(TAG, "*** MAX REBOOT ATTEMPTS (%d) REACHED ***", MAX_REBOOT_ATTEMPTS);
        ESP_LOGE(TAG, "*** SYSTEM HALTED - POWER CYCLE REQUIRED ***");
        ESP_LOGE(TAG, "*** Check hardware connections and wiring ***");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGW(TAG, "Rebooting in %d seconds... (attempt %lu of %d)",
             REBOOT_DELAY_MS / 1000,
             (unsigned long)s_reboot_count,
             MAX_REBOOT_ATTEMPTS);

    /* FAULT 10: record the reboot attempt before calling esp_restart()
     * so the fault is in the buffer for the next boot's first packet.
     * (RTC memory preserves the fault_log buffer across soft reboots.) */
    fault_log_record(FAULT_REBOOT_ATTEMPT);

    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));

    ESP_LOGW(TAG, "Rebooting now...");
    esp_restart();
}

/* -------------------------------------------------------------------------- */
/* Statistics Task                                                            */
/* -------------------------------------------------------------------------- */

static void stats_monitor_task(void *pvParameters)
{
    (void)pvParameters;

    uint32_t samples_published;
    uint32_t packets_sent;
    uint32_t samples_dropped;
    uint32_t acquired;
    uint32_t dropped;
    uint32_t max_time;
    esp_netif_ip_info_t ip_info;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_MS));

        data_processing_and_mqtt_task_get_stats(
            &samples_published, &packets_sent, &samples_dropped);
        sensor_acquisition_get_stats(&acquired, &dropped, &max_time);

        ESP_LOGI("STATS", "");
        ESP_LOGI("STATS", "============ System Statistics ============");

        ESP_LOGI("STATS", "--- ISR Acquisition ---");
        ESP_LOGI("STATS", "  ADXL355 samples:  %lu", (unsigned long)adxl355_get_sample_count());
        ESP_LOGI("STATS", "  ADXL355 overflow: %lu", (unsigned long)adxl355_get_overflow_count());
        ESP_LOGI("STATS", "  SCL3300 samples:  %lu", (unsigned long)scl3300_get_sample_count());
        ESP_LOGI("STATS", "  SCL3300 overflow: %lu", (unsigned long)scl3300_get_overflow_count());
        ESP_LOGI("STATS", "  ADT7420 samples:  %lu", (unsigned long)adt7420_get_sample_count());
        ESP_LOGI("STATS", "  Total acquired:   %lu", (unsigned long)acquired);
        ESP_LOGI("STATS", "  Total dropped:    %lu", (unsigned long)dropped);

        ESP_LOGI("STATS", "--- Ring Buffers ---");
        ESP_LOGI("STATS", "  ADXL355 pending:  %lu", (unsigned long)adxl355_samples_available());
        ESP_LOGI("STATS", "  SCL3300 pending:  %lu", (unsigned long)scl3300_samples_available());
        ESP_LOGI("STATS", "  ADT7420 pending:  %lu", (unsigned long)adt7420_samples_available());

        ESP_LOGI("STATS", "--- MQTT Publishing ---");
        ESP_LOGI("STATS", "  Samples published: %lu", (unsigned long)samples_published);
        ESP_LOGI("STATS", "  Packets sent:      %lu", (unsigned long)packets_sent);
        ESP_LOGI("STATS", "  Samples dropped:   %lu", (unsigned long)samples_dropped);

        ESP_LOGI("STATS", "--- Network ---");
        if (ethernet_is_connected()) {
            ethernet_get_ip_info(&ip_info);
            ESP_LOGI("STATS", "  Ethernet: Connected (" IPSTR ")", IP2STR(&ip_info.ip));
        } else {
            ESP_LOGW("STATS", "  Ethernet: Disconnected");
        }

        ESP_LOGI("STATS", "  MQTT: %s", mqtt_is_connected() ? "Connected" : "Disconnected");
        ESP_LOGI("STATS", "--- System ---");
        ESP_LOGI("STATS", "  Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
        ESP_LOGI("STATS", "  Tick count: %lu", (unsigned long)get_tick_count());
        ESP_LOGI("STATS", "=============================================");
        ESP_LOGI("STATS", "");
    }
}

/* -------------------------------------------------------------------------- */
/* Initialization                                                             */
/* -------------------------------------------------------------------------- */

static void print_banner(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  Wind Turbine Structural Health Monitor");
    ESP_LOGI(TAG, "  ISR Acquisition + MQTT Publishing");
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Architecture:");
    ESP_LOGI(TAG, "  - ISR at 8000 Hz (sensor acquisition)");
    ESP_LOGI(TAG, "  - ADXL355: 1000 Hz accelerometer");
    ESP_LOGI(TAG, "  - SCL3300: 20 Hz inclinometer");
    ESP_LOGI(TAG, "  - ADT7420: 1 Hz temperature");
    ESP_LOGI(TAG, "  - Ring buffers for lock-free data transfer");
    ESP_LOGI(TAG, "  - Data processing task batches & publishes");
    ESP_LOGI(TAG, "  - MQTT to Raspberry Pi");
    ESP_LOGI(TAG, "");
}

static esp_err_t init_buses(void)
{
    ESP_LOGI(TAG, "--- Initializing Buses ---");

    if (i2c_bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "I2C bus initialized");

    if (spi_bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SPI bus initialized");

    return ESP_OK;
}

static esp_err_t init_network(void)
{
    ESP_LOGI(TAG, "--- Initializing Network ---");

    if (ethernet_init() != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Waiting for IP address (timeout: %d sec)...", ETH_IP_TIMEOUT_MS / 1000);

    if (ethernet_wait_for_ip(ETH_IP_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "No IP address yet - will retry in background");
    } else {
        esp_netif_ip_info_t ip_info;
        ethernet_get_ip_info(&ip_info);
        ESP_LOGI(TAG, "Network ready: " IPSTR, IP2STR(&ip_info.ip));
    }

    return ESP_OK;
}

static esp_err_t init_mqtt(void)
{
    ESP_LOGI(TAG, "--- Initializing MQTT ---");

    esp_err_t mdns_ret = mqtt_mdns_init(ethernet_get_netif());
    if (mdns_ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed (broker hostname resolution may fail): %s",
                 esp_err_to_name(mdns_ret));
    }

    if (mqtt_init() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Waiting for MQTT connection (timeout: %d sec)...", MQTT_CONNECT_TIMEOUT_MS / 1000);

    if (mqtt_wait_for_connection(MQTT_CONNECT_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGI(TAG, "MQTT connected!");
        mqtt_publish_status("Wind Turbine Monitor Online");
    } else {
        ESP_LOGW(TAG, "MQTT connection timeout - will retry in background");
    }

    return ESP_OK;
}

/**
 * @brief Initialize sensors and record per-sensor fault codes on failure.
 *
 * ADT7420 is non-critical — failure is logged as FAULT_ADT7420_INIT_FAIL (16)
 * but init continues. ADXL355 and SCL3300 are critical — failure is logged
 * as FAULT_ADXL355_INIT_FAIL (14) or FAULT_SCL3300_INIT_FAIL (15) and then
 * handle_critical_failure() is called.
 */
static esp_err_t init_sensors(bool *temp_available)
{
    esp_err_t ret;
    *temp_available = false;

    ESP_LOGI(TAG, "--- Initializing Sensors ---");

    force_spi_cs_high_early();

    // ADT7420 Temperature Sensor (non-critical)
    ESP_LOGI(TAG, "Initializing ADT7420 temperature sensor...");
    ret = adt7420_init();
    if (ret == ESP_OK) {
        *temp_available = true;
        ESP_LOGI(TAG, "ADT7420 initialized");
    } else {
        /* FAULT 16: ADT7420 init failed */
        fault_log_record(FAULT_ADT7420_INIT_FAIL);
        ESP_LOGW(TAG, "ADT7420 init failed (fault 16 recorded) - continuing without temperature");
    }

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
#endif

    // ADXL355 Accelerometer (critical)
    ESP_LOGI(TAG, "Initializing ADXL355 accelerometer...");
    ret = adxl355_init();
    if (ret != ESP_OK) {
        /* FAULT 14: ADXL355 init failed */
        fault_log_record(FAULT_ADXL355_INIT_FAIL);
        ESP_LOGE(TAG, "ADXL355 init failed (fault 14 recorded) - CRITICAL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ADXL355 initialized");

#ifdef SPI_CS_ADXL355_IO
    gpio_set_level(SPI_CS_ADXL355_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
#endif

    // SCL3300 Inclinometer (critical)
    ESP_LOGI(TAG, "Initializing SCL3300 inclinometer...");
    ret = scl3300_init();
    if (ret != ESP_OK) {
        /* FAULT 15: SCL3300 init failed */
        fault_log_record(FAULT_SCL3300_INIT_FAIL);
        ESP_LOGE(TAG, "SCL3300 init failed (fault 15 recorded) - CRITICAL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SCL3300 initialized");

    return ESP_OK;
}

static esp_err_t init_acquisition(bool temp_available)
{
    ESP_LOGI(TAG, "--- Initializing ISR Acquisition ---");

    esp_err_t ret = sensor_acquisition_init(temp_available);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor acquisition init failed");
        return ESP_FAIL;
    }

    ret = sensor_acquisition_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor acquisition start failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ISR acquisition started (8000 Hz base rate)");
    return ESP_OK;
}

static esp_err_t init_data_processing(void)
{
    ESP_LOGI(TAG, "--- Initializing Data Processing Task ---");

    esp_err_t ret = data_processing_and_mqtt_task_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Data processing task init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Data processing task started");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* app_main                                                                   */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    bool temp_sensor_available = false;

    /* =========================================
     * 0. Check reset reason — MUST be first.
     *    Records boot-time fault codes (power loss, watchdog, reboot)
     *    into the pending buffer before anything else runs.
     *    These will be attached to the first outgoing MQTT data packet.
     * ========================================= */
    check_reset_reason();

    // Initialize reboot counter
    init_reboot_counter();

    print_banner();

    // =========================================
    // 1. Initialize Network (Ethernet)
    // =========================================
    if (init_network() != ESP_OK) {
        ESP_LOGE(TAG, "Network init failed - continuing anyway");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 2. Initialize MQTT Client
    // =========================================
    if (init_mqtt() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed - continuing anyway");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 3. Initialize Communication Buses
    // =========================================
    if (init_buses() != ESP_OK) {
        handle_critical_failure("Bus initialization failed (I2C or SPI)");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 4. Initialize Sensors
    // =========================================
    if (init_sensors(&temp_sensor_available) != ESP_OK) {
        handle_critical_failure("Critical sensor initialization failed (ADXL355 or SCL3300)");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 5. Initialize ISR Acquisition
    // =========================================
    if (init_acquisition(temp_sensor_available) != ESP_OK) {
        handle_critical_failure("ISR acquisition initialization failed");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 6. Initialize Data Processing + MQTT Task
    // =========================================
    if (init_data_processing() != ESP_OK) {
        handle_critical_failure("Data processing task initialization failed");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // Initialization Complete - Clear Reboot Counter
    // =========================================
    clear_reboot_counter();

    // =========================================
    // 7. Create Statistics Monitor Task
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
    ESP_LOGI(TAG, "Statistics monitor created (interval: %d sec)", STATS_INTERVAL_MS / 1000);
    ESP_LOGI(TAG, "");

    // =========================================
    // System Running
    // =========================================
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  SYSTEM RUNNING");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Data Flow:");
    ESP_LOGI(TAG, "  Sensors → ISR → Ring Buffers → Task → MQTT");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Subscribe on Raspberry Pi:");
    ESP_LOGI(TAG, "  mosquitto_sub -t \"wind_turbine/#\" -v");
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "");
}
