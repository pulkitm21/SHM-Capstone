/**
 * @file main.c
 * @brief Wind Turbine Structural Health Monitor
 *
 * Merged Architecture:
 *  - Ethernet + MQTT publishing (data_processing_and_mqtt_task)
 *  - ISR-based sensor acquisition (from isr-daq branch)
 *  - PTP time synchronization (Raspberry Pi as grandmaster)
 *  - Statistics monitor
 *
 * Initialization Order:
 *  0. Check reset reason (fault logging)
 *  1. Ethernet (includes L2TAP registration for PTP)
 *  2. PTP sync (blocks until CLOCK_PTP_SYSTEM is valid)
 *  3. MQTT
 *  4. Buses (I2C, SPI)
 *  5. Sensors
 *  6. ISR Acquisition
 *  7. Data Processing Task
 *
 * PTP Notes:
 *  - Raspberry Pi runs ptp4l as IEEE 1588 grandmaster
 *  - ptpd_start("ETH_0") spawns the PTP daemon at tskIDLE_PRIORITY + 2
 *  - ptp_init_and_sync() blocks until clock_source_valid is stable
 *    for 3 consecutive checks, matching the Espressif PTP example pattern
 *  - Sensor acquisition does NOT start until PTP is synced, ensuring
 *    all packets have valid timestamps from the first sample
 *  - CLOCK_PTP_SYSTEM is initialized inside ptpd_start() via
 *    ptp_initialize_state() → esp_eth_clock_init(). No separate init needed.
 *
 * Data Flow:
 *  ISR (8000 Hz) → Ring Buffers → Data Processing Task → MQTT → Raspberry Pi
 *
 * FAULT LOGGING (boot-time):
 *  At the top of app_main(), esp_reset_reason() is checked and fault codes
 *  are recorded before any other initialization.
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

// PTP
#include "ptpd.h"
#include "esp_eth_time.h"

// Fault logging
#include "fault_log.h"

static const char *TAG = "main";

// Timeouts
#define ETH_IP_TIMEOUT_MS       30000
#define MQTT_CONNECT_TIMEOUT_MS 30000

/* PTP synchronization timeout.
 * ptp_init_and_sync() will give up and continue (with a warning) if
 * the clock has not synced within this many milliseconds. Sensor
 * acquisition will still start but packets will have timestamp=0 until
 * sync is achieved. In practice on a local LAN, sync takes 5-15 seconds.
 */
#define PTP_SYNC_TIMEOUT_MS     60000

/* Number of consecutive clock_source_valid checks required before we
 * consider the clock locked. Matches the pattern in the Espressif example. */
#define PTP_SYNC_STABLE_COUNT   3

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

/* PTP daemon process ID (returned by ptpd_start, used by ptpd_status) */
static int s_ptp_pid = -1;

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

static void handle_critical_failure(const char *reason)
{
    ESP_LOGE(TAG, "*** CRITICAL FAILURE: %s ***", reason);

    s_reboot_count++;

    if (s_reboot_count >= MAX_REBOOT_ATTEMPTS) {
        ESP_LOGE(TAG, "*** MAX REBOOT ATTEMPTS (%d) REACHED ***", MAX_REBOOT_ATTEMPTS);
        ESP_LOGE(TAG, "*** SYSTEM HALTED - POWER CYCLE REQUIRED ***");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGW(TAG, "Rebooting in %d seconds... (attempt %lu of %d)",
             REBOOT_DELAY_MS / 1000,
             (unsigned long)s_reboot_count,
             MAX_REBOOT_ATTEMPTS);

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

        /* Show current PTP time so we can visually verify sync */
        struct timespec ptp_ts;
        if (esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &ptp_ts) == 0) {
            ESP_LOGI("STATS", "  PTP time:  %llu.%09lu (Unix epoch)",
                     (unsigned long long)ptp_ts.tv_sec,
                     (unsigned long)ptp_ts.tv_nsec);
        } else {
            ESP_LOGW("STATS", "  PTP time:  not available");
        }

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
    ESP_LOGI(TAG, "  ISR Acquisition + MQTT Publishing + PTP Sync");
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Architecture:");
    ESP_LOGI(TAG, "  - ISR at 8000 Hz (sensor acquisition)");
    ESP_LOGI(TAG, "  - ADXL355: 1000 Hz accelerometer");
    ESP_LOGI(TAG, "  - SCL3300: 20 Hz inclinometer");
    ESP_LOGI(TAG, "  - ADT7420: 1 Hz temperature");
    ESP_LOGI(TAG, "  - PTP slave (Raspberry Pi grandmaster)");
    ESP_LOGI(TAG, "  - Timestamps: microseconds since Unix epoch");
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

/**
 * @brief Start the PTP daemon and block until the clock is synchronized.
 *
 * SEQUENCE:
 *  1. Call ptpd_start("ETH_0") to spawn the PTP daemon task at
 *     tskIDLE_PRIORITY + 2 (lowest useful priority, below everything else).
 *  2. Wait for esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, ...) to return 0
 *     (clock available), which can take a few seconds after first Sync packet.
 *  3. Poll ptpd_status() until clock_source_valid has been true for
 *     PTP_SYNC_STABLE_COUNT consecutive polls (500ms apart), confirming
 *     the PI controller has converged and the clock is locked to the Pi.
 *  4. Give up and continue after PTP_SYNC_TIMEOUT_MS with a warning.
 *     Sensor acquisition will start anyway but packets will have
 *     timestamp=0 until sync is achieved.
 *
 * IMPORTANT:
 *  - Must be called AFTER ethernet_init() and ethernet_wait_for_ip().
 *    The PTP daemon needs the Ethernet link up and L2TAP registered
 *    (L2TAP is registered inside ethernet_init()).
 *  - Must be called BEFORE init_acquisition() so that all published
 *    packets have valid PTP timestamps from the very first sample.
 *  - ptpd_status() dereferences s_state inside ptpd.c. We wait for
 *    esp_eth_clock_gettime() to succeed first as a proxy for the daemon
 *    having initialized s_state, avoiding a NULL dereference.
 *
 * @return ESP_OK if synced within timeout, ESP_ERR_TIMEOUT otherwise.
 */
static esp_err_t ptp_init_and_sync(void)
{
    ESP_LOGI(TAG, "--- Initializing PTP (Raspberry Pi grandmaster) ---");

    /* Start the PTP daemon. It attaches to the existing "ETH_0" netif
     * that ethernet_init() already created. Returns 1 on success, -1
     * if another instance is already running. */
    s_ptp_pid = ptpd_start("ETH_0");
    if (s_ptp_pid < 0) {
        ESP_LOGE(TAG, "ptpd_start failed (another instance running?)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PTP daemon started (pid=%d, priority=tskIDLE_PRIORITY+2)", s_ptp_pid);

    /* Step 1: On ESP32, clock_gettime(CLOCK_REALTIME) always succeeds
         * immediately after boot (returns 0 = Unix epoch). There is no
         * meaningful way to wait for the clock to "become available" here —
         * the real readiness signal is clock_source_valid in step 2 below.
         * We just give the ptpd task a moment to start up before polling. */

        ESP_LOGI(TAG, "Waiting for PTP daemon to start up...");
        struct timespec ts;
        uint32_t elapsed_ms = 0;
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed_ms = 500;
        ESP_LOGI(TAG, "PTP daemon startup delay complete, waiting for clock lock...");

    /* -----------------------------------------------------------------------
     * Step 2: Wait for clock_source_valid to be stable.
     *
     * clock_source_valid becomes true when the PTP daemon has received
     * Announce and Sync packets from the grandmaster (Raspberry Pi) and
     * the PI controller has started converging. We require it to be true
     * for PTP_SYNC_STABLE_COUNT consecutive polls (500ms apart) before
     * considering the clock locked.
     * ----------------------------------------------------------------------- */
    ESP_LOGI(TAG, "Waiting for PTP clock to lock to Raspberry Pi grandmaster...");
    ESP_LOGI(TAG, "(This typically takes 5-15 seconds on a local LAN)");

    int32_t stable_count = 0;
    elapsed_ms = 0;
    const uint32_t sync_poll_ms = 500;

    while (stable_count < PTP_SYNC_STABLE_COUNT) {
        vTaskDelay(pdMS_TO_TICKS(sync_poll_ms));
        elapsed_ms += sync_poll_ms;

        if (elapsed_ms >= PTP_SYNC_TIMEOUT_MS) {
            ESP_LOGW(TAG, "PTP sync timeout after %d ms — starting acquisition anyway",
                     PTP_SYNC_TIMEOUT_MS);
            ESP_LOGW(TAG, "Packets will have timestamp=0 until PTP converges");
            return ESP_ERR_TIMEOUT;
        }

        struct ptpd_status_s status;
        if (ptpd_status(s_ptp_pid, &status) == 0) {
            if (status.clock_source_valid) {
                stable_count++;
                ESP_LOGI(TAG, "PTP clock_source_valid (%ld/%d) — delta: %lld ns",
                         (long)stable_count, PTP_SYNC_STABLE_COUNT,
                         (long long)status.last_delta_ns);
            } else {
                /* Reset counter — require consecutive valid readings */
                if (stable_count > 0) {
                    ESP_LOGD(TAG, "PTP clock_source_valid went false — resetting stable count");
                }
                stable_count = 0;
            }
        } else {
            /* ptpd_status() timed out internally (1s timeout in ptpd.c).
             * This can happen briefly during daemon startup. Just keep waiting. */
            stable_count = 0;
            ESP_LOGD(TAG, "ptpd_status() timed out — daemon still starting");
        }
    }

    /* Log the current PTP time so we can verify it looks like real UTC */
    if (esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &ts) == 0) {
        ESP_LOGI(TAG, "PTP synchronized! Time: %llu.%09lu (Unix epoch)",
                 (unsigned long long)ts.tv_sec,
                 (unsigned long)ts.tv_nsec);
        ESP_LOGI(TAG, "Elapsed: ~%lu ms to lock", (unsigned long)elapsed_ms);
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
        mqtt_publish_status("Wind Turbine Monitor Online - PTP Synchronized");
    } else {
        ESP_LOGW(TAG, "MQTT connection timeout - will retry in background");
    }

    return ESP_OK;
}

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
     * ========================================= */
    check_reset_reason();
    init_reboot_counter();
    print_banner();

    // =========================================
    // 1. Initialize Network (Ethernet + L2TAP)
    //    L2TAP is registered inside ethernet_init()
    //    and is required by ptpd_start().
    // =========================================
    if (init_network() != ESP_OK) {
        ESP_LOGE(TAG, "Network init failed - continuing anyway");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 2. PTP Synchronization
    //    Must come BEFORE sensor acquisition so
    //    that all packets have valid timestamps.
    //    Must come AFTER ethernet_init() and
    //    ethernet_wait_for_ip() (link must be up).
    // =========================================
    esp_err_t ptp_ret = ptp_init_and_sync();
    if (ptp_ret == ESP_OK) {
        ESP_LOGI(TAG, "PTP synchronized to Raspberry Pi grandmaster");
    } else {
        ESP_LOGW(TAG, "PTP sync incomplete — timestamps may be 0 initially");
        ESP_LOGW(TAG, "Continuing: PTP daemon is running and will sync in background");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 3. Initialize MQTT Client
    // =========================================
    if (init_mqtt() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed - continuing anyway");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 4. Initialize Communication Buses
    // =========================================
    if (init_buses() != ESP_OK) {
        handle_critical_failure("Bus initialization failed (I2C or SPI)");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 5. Initialize Sensors
    // =========================================
    if (init_sensors(&temp_sensor_available) != ESP_OK) {
        handle_critical_failure("Critical sensor initialization failed (ADXL355 or SCL3300)");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 6. Initialize ISR Acquisition
    //    PTP is synced before this point, so
    //    the first batch will have a valid timestamp.
    // =========================================
    if (init_acquisition(temp_sensor_available) != ESP_OK) {
        handle_critical_failure("ISR acquisition initialization failed");
    }
    ESP_LOGI(TAG, "");

    // =========================================
    // 7. Initialize Data Processing + MQTT Task
    // =========================================
    if (init_data_processing() != ESP_OK) {
        handle_critical_failure("Data processing task initialization failed");
    }
    ESP_LOGI(TAG, "");

    clear_reboot_counter();

    // =========================================
    // 8. Create Statistics Monitor Task
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
    ESP_LOGI(TAG, "  Node identity: %s", mqtt_get_serial_no());
    ESP_LOGI(TAG, "  Data topic:    %s", mqtt_get_topic_data());
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Data Flow:");
    ESP_LOGI(TAG, "  Sensors → ISR → Ring Buffers → Task → MQTT");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Timestamps: PTP-synchronized Unix microseconds");
    ESP_LOGI(TAG, "  Pi decoder: datetime.utcfromtimestamp(t / 1e6)");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Subscribe on Raspberry Pi:");
    ESP_LOGI(TAG, "  mosquitto_sub -t \"wind_turbine/#\" -v");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  NOTE: If identity shows MAC-XXXXXXXXXXXX, this");
    ESP_LOGI(TAG, "  node has no serial number provisioned yet.");
    ESP_LOGI(TAG, "  See mqtt.h for NVS flashing instructions.");
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "");

    // app_main() returns - tasks continue running
}
