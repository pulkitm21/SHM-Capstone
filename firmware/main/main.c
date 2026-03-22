/**
 * @file main.c
 * @brief Wind Turbine Structural Health Monitor
 *
 * Merged Architecture:
 *  - Ethernet + MQTT publishing (data_processing_and_mqtt_task)
 *  - ISR-based sensor acquisition
 *  - Statistics monitor
 *
 * Error Handling:
 *  - Critical failures trigger automatic reboot after delay
 *  - Maximum reboot attempts tracked in memory
 *  - Prevents infinite reboot loops
 *
 * Data Flow:
 *  ISR (8000 Hz) -> Ring Buffers -> Data Processing Task -> MQTT -> Raspberry Pi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Node state machine + runtime configuration
#include "node_config.h"
#include "fault_log.h"
#include "sntp_sync.h"

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
 * @brief Initialize reboot counter from RTC memory
 */
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

/**
 * @brief Clear reboot counter after successful initialization
 */
static void clear_reboot_counter(void)
{
    s_reboot_count = 0;
    ESP_LOGI(TAG, "Initialization successful - reboot counter cleared");
}

/**
 * @brief Handle critical failure: reboot or halt
 *
 * @param reason Description of what failed
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
    } else {
        ESP_LOGW(TAG, "Rebooting in %d seconds... (attempt %lu of %d)",
                 REBOOT_DELAY_MS / 1000,
                 (unsigned long)s_reboot_count,
                 MAX_REBOOT_ATTEMPTS);

        vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));

        ESP_LOGW(TAG, "Rebooting now...");
        esp_restart();
    }
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
            &samples_published,
            &packets_sent,
            &samples_dropped
        );

        sensor_acquisition_get_stats(&acquired, &dropped, &max_time);

        ESP_LOGI("STATS", "");
        ESP_LOGI("STATS", "============ System Statistics ============");

        ESP_LOGI("STATS", "--- Node State ---");
        ESP_LOGI("STATS", "  State: %s", node_state_str(node_config_get_state()));
        const node_runtime_config_t *cfg = node_config_get();
        ESP_LOGI("STATS", "  ODR:   %lu Hz", (unsigned long)cfg->odr_hz);
        ESP_LOGI("STATS", "  Range: +/-%dg", (cfg->range == 1) ? 2 : (cfg->range == 2) ? 4 : 8);
        ESP_LOGI("STATS", "  Decim: %lu  Batch: %lu",
                 (unsigned long)cfg->decim_factor, (unsigned long)cfg->batch_size);

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

        if (mqtt_is_connected()) {
            ESP_LOGI("STATS", "  MQTT: Connected");
        } else {
            ESP_LOGW("STATS", "  MQTT: Disconnected");
        }

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
        ESP_LOGE(TAG, "Ethernet driver init failed (check PHY wiring / SPI bus)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Waiting for IP address (timeout: %d sec)...", ETH_IP_TIMEOUT_MS / 1000);

    if (ethernet_wait_for_ip(ETH_IP_TIMEOUT_MS) != ESP_OK) {
        // Hard failure — no point trying MQTT without an IP.
        // Common causes:
        //   - Cable not plugged in
        //   - Switch/router not providing DHCP
        //   - PHY reset GPIO wrong (check PHY_RESET_GPIO in ethernet.c)
        //   - ethernet_init() called before SPI bus was ready (should be fixed
        //     by the current main.c init order: buses -> sensors -> network)
        ESP_LOGE(TAG, "No IP address obtained after %d sec", ETH_IP_TIMEOUT_MS / 1000);
        ESP_LOGE(TAG, "Check: cable, DHCP server, PHY reset GPIO, SPI bus init order");
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    ethernet_get_ip_info(&ip_info);
    ESP_LOGI(TAG, "Network ready: " IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

static esp_err_t init_mqtt(void)
{
    ESP_LOGI(TAG, "--- Initializing MQTT ---");

    // mDNS must be initialized AFTER we have an IP so the stack is bound to
    // a live netif.  We guard on ethernet_is_connected() which is only true
    // once ETH_GOT_IP_BIT is set (i.e. init_network() succeeded).
    if (!ethernet_is_connected()) {
        ESP_LOGE(TAG, "Cannot init MQTT - no IP address (ethernet not connected)");
        return ESP_FAIL;
    }

    // mqtt_mdns_init also calls build_identity_strings() to resolve the NVS
    // serial number.  mqtt_init() calls it again harmlessly, but mDNS must
    // come first so the hostname is advertised before the broker connection
    // is attempted.
    esp_err_t ret = mqtt_mdns_init(ethernet_get_netif());
    if (ret != ESP_OK) {
        // Non-fatal: mDNS failing means "raspberrypi.local" won't resolve.
        // If the broker is reachable by IP this will still work, but log
        // clearly so the user knows hostname resolution is broken.
        ESP_LOGW(TAG, "mDNS init failed - broker hostname may not resolve: %s",
                 esp_err_to_name(ret));
        ESP_LOGW(TAG, "If MQTT fails, set MQTT_BROKER_URI to a static IP in mqtt.h");
    }

    if (mqtt_init() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Waiting for MQTT broker connection (timeout: %d sec)...",
             MQTT_CONNECT_TIMEOUT_MS / 1000);

    if (mqtt_wait_for_connection(MQTT_CONNECT_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGI(TAG, "MQTT connected to broker");
        mqtt_publish_status("Wind Turbine Monitor Online");
    } else {
        // Non-fatal: the MQTT client will keep retrying in the background
        // (reconnect_timeout_ms = 5000 in mqtt.c).  Data will be published
        // once the connection is established.
        ESP_LOGW(TAG, "MQTT broker connection timeout after %d sec",
                 MQTT_CONNECT_TIMEOUT_MS / 1000);
        ESP_LOGW(TAG, "Check: broker running on Pi? 'raspberrypi.local' resolves?");
        ESP_LOGW(TAG, "MQTT client will keep retrying automatically in the background");
    }

    return ESP_OK;
}

static esp_err_t init_sntp(void)
{
    ESP_LOGI(TAG, "--- Initializing SNTP ---");
    if (!ethernet_is_connected()) {
        ESP_LOGW(TAG, "Skipping SNTP init - no network");
        return ESP_FAIL;
    }
    esp_err_t ret = sntp_sync_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed (%s) - timestamps will be tick-relative",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SNTP client started (server: %s)", SNTP_SERVER_HOSTNAME);
    }
    return ret;
}

static esp_err_t init_sensors(bool *temp_available)
{
    esp_err_t ret;
    *temp_available = false;

    ESP_LOGI(TAG, "--- Initializing Sensors ---");

    /*
     * Important CS logic:
     * - ADXL355 uses automatic CS in its SPI device config
     * - SCL3300 uses manual CS
     * - Only do an early safety deselect of both lines here
     * - Do NOT keep manually toggling ADXL355 CS after init
     */
    force_spi_cs_high_early();

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
#endif

    ESP_LOGI(TAG, "Initializing ADT7420 temperature sensor...");
    ret = adt7420_init();
    if (ret == ESP_OK) {
        *temp_available = true;
        ESP_LOGI(TAG, "ADT7420 initialized");
    } else {
        ESP_LOGW(TAG, "ADT7420 init failed - continuing without temperature");
    }

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
#endif

    ESP_LOGI(TAG, "Initializing ADXL355 accelerometer...");
    ret = adxl355_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADXL355 init failed - CRITICAL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ADXL355 initialized");

    /*
     * Do NOT manually touch SPI_CS_ADXL355_IO here.
     * ADXL355 CS is handled automatically by the SPI driver.
     * Only keep SCL3300 deselected before its own init.
     */
#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
#endif

    ESP_LOGI(TAG, "Initializing SCL3300 inclinometer...");
    ret = scl3300_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCL3300 init failed - CRITICAL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SCL3300 initialized");

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
#endif

    return ESP_OK;
}

static esp_err_t init_acquisition(bool temp_available)
{
    ESP_LOGI(TAG, "--- Initializing ISR Acquisition ---");

    /*
     * sensor_acquisition_init() sets up the GPTimer and ring buffers but does
     * NOT start the timer. The ISR only starts when sensor_acquisition_start()
     * is called via the MQTT command handler (start command from Pi).
     * This is the IDLE boot behaviour.
     */
    esp_err_t ret = sensor_acquisition_init(temp_available);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor acquisition init failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ISR acquisition ready (timer NOT started — node is IDLE)");
    ESP_LOGI(TAG, "Send configure + start commands via MQTT to begin recording.");
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
/* MQTT Command Handler                                                       */
/* -------------------------------------------------------------------------- */

static int32_t json_get_int(const char *json, const char *key, int32_t default_val)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long val = strtol(p, &end, 10);
    if (end == p) return default_val;
    return (int32_t)val;
}

static bool json_str_equals(const char *json, const char *key, const char *expected)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t exp_len = strlen(expected);
    return (strncmp(p, expected, exp_len) == 0 &&
            (p[exp_len] == '"' || p[exp_len] == '\0'));
}

static void publish_node_status(uint32_t seq_ack, bool selftest_ok,
                                 const char *error_msg)
{
    const node_runtime_config_t *cfg = node_config_get();
    mqtt_publish_status_json(
        node_state_str(node_config_get_state()),
        cfg->odr_hz, cfg->range, 200,
        cfg->hpf_corner,
        selftest_ok, seq_ack, error_msg
    );
}

static void on_mqtt_cmd(const char *topic, const char *payload)
{
    ESP_LOGI("CMD", "Received on [%s]: %s", topic, payload);
    node_state_t state = node_config_get_state();

    /* ---- configure ---- */
    if (strstr(topic, "/cmd/configure")) {
        if (state == NODE_STATE_ERROR) {
            publish_node_status(0, false, "node in error state, send reset first");
            return;
        }

        int32_t odr_index  = json_get_int(payload, "odr_index",  2);
        int32_t range      = json_get_int(payload, "range",      1);
        int32_t hpf_corner = json_get_int(payload, "hpf_corner", 0);
        int32_t seq        = json_get_int(payload, "seq",        0);

        if (odr_index < 0 || odr_index > 2) {
            publish_node_status((uint32_t)seq, false, "invalid odr_index");
            return;
        }
        if (range < 1 || range > 3) {
            publish_node_status((uint32_t)seq, false, "invalid range");
            return;
        }
        if (hpf_corner < 0 || hpf_corner > 6) {
            publish_node_status((uint32_t)seq, false, "invalid hpf_corner");
            return;
        }

        adxl355_selftest_result_t st_result;
        esp_err_t err = node_config_apply(
            (uint8_t)odr_index, (uint8_t)range,
            (uint8_t)hpf_corner, (uint32_t)seq,
            &st_result
        );

        if (err != ESP_OK) {
            publish_node_status((uint32_t)seq, false, "register write failed");
            return;
        }

        /* If node was recording before reconfiguration, restart ISR */
        if (state == NODE_STATE_RECORDING) {
            esp_err_t start_err = sensor_acquisition_start();
            if (start_err != ESP_OK) {
                node_config_set_error(FAULT_SPI_ERROR);
                publish_node_status((uint32_t)seq, false, "ISR restart failed");
                return;
            }
            node_config_set_recording();
            ESP_LOGI("CMD", "Reconfiguration complete — recording resumed");
        }

        publish_node_status((uint32_t)seq, st_result.passed, NULL);
        return;
    }

    /* ---- control ---- */
    if (strstr(topic, "/cmd/control")) {

        if (json_str_equals(payload, "cmd", "start")) {
            if (state == NODE_STATE_CONFIGURED) {
                esp_err_t err = sensor_acquisition_start();
                if (err != ESP_OK) {
                    node_config_set_error(FAULT_SPI_ERROR);
                    publish_node_status(0, false, "ISR start failed");
                    return;
                }
                node_config_set_recording();
                publish_node_status(0, true, NULL);
            } else if (state == NODE_STATE_RECORDING) {
                ESP_LOGW("CMD", "Already recording");
                publish_node_status(0, true, NULL);
            } else {
                ESP_LOGW("CMD", "Cannot start from state '%s' — send configure first",
                         node_state_str(state));
                publish_node_status(0, false, "must configure before start");
            }
            return;
        }

        if (json_str_equals(payload, "cmd", "stop")) {
            if (state == NODE_STATE_RECORDING) {
                sensor_acquisition_stop();
                node_config_set_configured();
            }
            publish_node_status(0, true, NULL);
            return;
        }

        if (json_str_equals(payload, "cmd", "init")) {
            if (state == NODE_STATE_RECORDING) {
                sensor_acquisition_stop();
            }
            adxl355_selftest_result_t st_result;
            esp_err_t err = node_config_apply(2, NODE_RANGE_2G, 0, 0, &st_result);
            if (err != ESP_OK) {
                publish_node_status(0, false, "init failed");
                return;
            }
            publish_node_status(0, st_result.passed, NULL);
            return;
        }

        if (json_str_equals(payload, "cmd", "reset")) {
            if (state == NODE_STATE_RECORDING) {
                sensor_acquisition_stop();
            }
            node_config_reset();
            publish_node_status(0, true, NULL);
            return;
        }

        ESP_LOGW("CMD", "Unknown control cmd: %s", payload);
        return;
    }

    ESP_LOGW("CMD", "Unrecognised cmd topic: %s", topic);
}

/* -------------------------------------------------------------------------- */
/* app_main                                                                   */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    bool temp_sensor_available = false;

    /* Initialise state machine first — sets state to IDLE, loads defaults. */
    node_config_init();

    init_reboot_counter();
    print_banner();

    /* Register cmd handler BEFORE mqtt_init so no message can arrive
     * before the handler is wired up. */
    mqtt_set_cmd_handler(on_mqtt_cmd);

    bool network_ok = (init_network() == ESP_OK);
    ESP_LOGI(TAG, "");

    bool mqtt_ok = false;
    if (network_ok) {
        mqtt_ok = (init_mqtt() == ESP_OK);
    } else {
        ESP_LOGW(TAG, "Skipping MQTT init - no network");
    }
    ESP_LOGI(TAG, "");

    bool sntp_ok = false;
    if (network_ok) {
        sntp_ok = (init_sntp() == ESP_OK);
    }
    ESP_LOGI(TAG, "");

    if (init_buses() != ESP_OK) {
        handle_critical_failure("Bus initialization failed (I2C or SPI)");
    }
    ESP_LOGI(TAG, "");

    if (init_sensors(&temp_sensor_available) != ESP_OK) {
        handle_critical_failure("Critical sensor initialization failed (ADXL355 or SCL3300)");
    }
    ESP_LOGI(TAG, "");

    // Wait for first SNTP sync before starting ISR so every sample gets
    // a real ISO-8601 timestamp from packet one. Hard timeout: 10 s.
    if (sntp_ok) {
        ESP_LOGI(TAG, "--- Waiting for first SNTP sync ---");
        const int SNTP_WAIT_TIMEOUT_MS = 10000;
        const int SNTP_WAIT_STEP_MS    = 100;
        int waited_ms = 0;
        while (!sntp_sync_is_valid() && waited_ms < SNTP_WAIT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(SNTP_WAIT_STEP_MS));
            waited_ms += SNTP_WAIT_STEP_MS;
        }
        if (sntp_sync_is_valid()) {
            ESP_LOGI(TAG, "SNTP synced after %d ms — ISR will use UTC timestamps",
                     waited_ms);
        } else {
            ESP_LOGW(TAG, "SNTP sync timeout (%d ms) — using tick-relative timestamps",
                     waited_ms);
        }
        ESP_LOGI(TAG, "");
    }

    if (init_acquisition(temp_sensor_available) != ESP_OK) {
        handle_critical_failure("ISR acquisition initialization failed");
    }
    ESP_LOGI(TAG, "");

    if (init_data_processing() != ESP_OK) {
        handle_critical_failure("Data processing task initialization failed");
    }
    ESP_LOGI(TAG, "");

    /* Subscribe to cmd topics. If not yet connected, the MQTT_EVENT_CONNECTED
     * handler will re-subscribe automatically when the connection is made. */
    if (mqtt_ok && mqtt_is_connected()) {
        mqtt_subscribe_cmd();
    }

    clear_reboot_counter();

    /* Publish initial IDLE status so the Pi knows the node is up */
    if (mqtt_ok) {
        publish_node_status(0, true, NULL);
    }

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

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  SYSTEM RUNNING — STATE: IDLE");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Waiting for configure + start MQTT commands.");
    ESP_LOGI(TAG, "  Network:  %s", network_ok ? "Connected" : "OFFLINE");
    ESP_LOGI(TAG, "  MQTT:     %s", mqtt_ok ? "Connected" : (network_ok ? "Connecting..." : "Disabled"));
    if (network_ok) {
        esp_netif_ip_info_t ip;
        ethernet_get_ip_info(&ip);
        ESP_LOGI(TAG, "  IP:       " IPSTR, IP2STR(&ip.ip));
        ESP_LOGI(TAG, "  Node ID:  %s", mqtt_get_serial_no());
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "  Configure: wind_turbine/%s/cmd/configure", mqtt_get_serial_no());
        ESP_LOGI(TAG, "  Control:   wind_turbine/%s/cmd/control",   mqtt_get_serial_no());
        ESP_LOGI(TAG, "  Status:    %s", mqtt_get_topic_status());
        ESP_LOGI(TAG, "  Data:      %s", mqtt_get_topic_data());
        if (!mqtt_ok) {
            ESP_LOGW(TAG, "");
            ESP_LOGW(TAG, "  MQTT not connected. Check mosquitto is running on Pi.");
            ESP_LOGW(TAG, "  Client retries every 5 sec automatically.");
        }
    }
    ESP_LOGI(TAG, "");
    if (network_ok) {
        ESP_LOGI(TAG, "  NOTE: If Node ID shows MAC-XXXXXXXXXXXX,");
        ESP_LOGI(TAG, "  this node has no serial number provisioned.");
        ESP_LOGI(TAG, "  See mqtt.h for NVS flashing instructions.");
    }
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "");
}