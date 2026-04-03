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

/******************************************************************************
 * FAULT PUBLISH CALLBACK
 * Registered with fault_log_set_publish_cb() after mqtt_init() succeeds.
 * Builds the full topic from the node serial and the FAULT_LOG_TOPIC_SUFFIX
 * defined in fault_log.h, then forwards to mqtt_publish().
 * Called from fault_log_record() on every fault, regardless of sensor state.
 ******************************************************************************/
static void on_fault_publish(const char *payload, int len)
{
    if (!mqtt_is_connected()) {
        return;
    }
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/%s/%s",
             MQTT_TOPIC_PREFIX,
             mqtt_get_serial_no(),
             FAULT_LOG_TOPIC_SUFFIX);
    esp_err_t ret = mqtt_publish(topic, payload, len);
    if (ret != ESP_OK) {
        ESP_LOGW("MAIN", "Immediate fault publish failed -- fault retained in pending buffer");
    }
}

static const char *TAG = "main";

static volatile bool s_mqtt_started = false;  /* written from event task, read from app_main -- must be volatile */

/**
 * @brief Called by ethernet.c every time an IP address is obtained.
 *
 * Handles two cases:
 *  - First IP at boot: ethernet_wait_for_ip() already returned in init_network(),
 *    but if that timed out (network_ok=false), MQTT was never started. This
 *    callback starts it now.
 *  - Subsequent IP after reconnect: MQTT client is already running and will
 *    reconnect to the broker on its own. Only SNTP may need a nudge if it
 *    never synced.
 */
static void on_ethernet_got_ip(esp_netif_t *netif)
{
    ESP_LOGI(TAG, "IP obtained -- checking MQTT/SNTP state");

    if (!s_mqtt_started) {
        ESP_LOGI(TAG, "MQTT not yet started -- initialising now");

        esp_err_t ret = mqtt_mdns_init(netif);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "mDNS init failed on late start: %s", esp_err_to_name(ret));
        }

        ret = mqtt_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MQTT init failed on late start -- will retry on next IP event");
            return;
        }

        /* Build the cmd topic strings so MQTT_EVENT_CONNECTED can subscribe
         * when the broker connection is established. Do NOT subscribe here --
         * the connection isn't up yet and MQTT_EVENT_CONNECTED will handle it. */
        mqtt_subscribe_cmd();

        s_mqtt_started = true;
        ESP_LOGI(TAG, "MQTT started after late IP acquisition");
    }
    /* If already started, the MQTT client handles broker reconnection itself
     * via its internal retry loop -- nothing to do here. */
}

// Timeouts
#define ETH_IP_TIMEOUT_MS       30000
#define MQTT_CONNECT_TIMEOUT_MS 30000

// Reboot configuration
#define REBOOT_DELAY_MS         5000
/* No cap on reboot attempts -- node reboots indefinitely on critical failure.
 * A power quality event may cause transient sensor init failures; halting
 * permanently would leave the node idle with no data. Sensor faults that
 * are non-critical (ADXL355/SCL3300/ADT7420 absent) do NOT reach here --
 * those are handled gracefully by the data processing task (NaN output). */

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
        ESP_LOGW(TAG, "Reboot detected - attempt %lu",
                 (unsigned long)(s_reboot_count + 1));
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
 * @brief Handle critical failure: always reboot after a delay.
 *
 * The node reboots indefinitely -- there is no halt. A transient hardware
 * or power quality event may clear on its own; a permanently idle node
 * collecting no data is worse than a node that keeps retrying.
 * The reboot counter is kept in RTC memory for logging purposes only.
 *
 * Non-critical sensor faults (ADXL355 / SCL3300 / ADT7420 disconnected)
 * do NOT reach this function -- those are handled by the data processing
 * task which sends NaN and keeps recording the remaining sensors.
 *
 * @param reason Description of what failed
 */
static void handle_critical_failure(const char *reason)
{
    ESP_LOGE(TAG, "*** CRITICAL FAILURE: %s ***", reason);

    s_reboot_count++;

    ESP_LOGW(TAG, "Rebooting in %d seconds... (attempt %lu)",
             REBOOT_DELAY_MS / 1000,
             (unsigned long)s_reboot_count);

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
        fault_log_record(FAULT_I2C_ERROR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "I2C bus initialized");

    if (spi_bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        fault_log_record(FAULT_SPI_ERROR);
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
        // Hard failure -- no point trying MQTT without an IP.
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

    ESP_LOGI(TAG, "--- Power-On Self-Test (POST) ---");

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

    /* POST result tracking */
    bool post_adxl355_ok  = false;
    bool post_scl3300_ok  = false;
    bool post_adt7420_ok  = false;

    /* ---- ADT7420 ---- */
    ESP_LOGI(TAG, "POST: ADT7420 temperature sensor...");
    ret = adt7420_init();
    if (ret == ESP_OK) {
        *temp_available = true;
        /* Self-test: read temperature and check it is physically plausible */
        float post_temp = 0.0f;
        esp_err_t st_err = adt7420_selftest(&post_temp);
        if (st_err == ESP_OK) {
            post_adt7420_ok = true;
            ESP_LOGI(TAG, "POST: ADT7420 PASS  (%.1f  degC)", post_temp);
        } else {
            post_adt7420_ok = false;
            ESP_LOGW(TAG, "POST: ADT7420 FAIL  (temperature out of range or read error)");
            fault_log_record(FAULT_ADT7420_INIT_FAIL);
        }
    } else {
        ESP_LOGW(TAG, "POST: ADT7420 ABSENT (NaN will be sent for temperature)");
        fault_log_record(FAULT_ADT7420_INIT_FAIL);
    }

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
#endif

    /* ---- ADXL355 ---- */
    ESP_LOGI(TAG, "POST: ADXL355 accelerometer...");
    ret = adxl355_init();
    if (ret == ESP_OK) {
        /* Self-test via existing node_config mechanism (default ODR=1kHz, +/-2g) */
        adxl355_selftest_result_t st;
        esp_err_t st_err = node_config_run_selftest(&st);
        if (st_err == ESP_OK && st.passed) {
            post_adxl355_ok = true;
            ESP_LOGI(TAG, "POST: ADXL355 PASS  (delta_X=%.3fg delta_Y=%.3fg delta_Z=%.3fg)",
                     st.delta_x, st.delta_y, st.delta_z);
        } else {
            post_adxl355_ok = false;
            ESP_LOGW(TAG, "POST: ADXL355 FAIL  (delta_X=%.3fg delta_Y=%.3fg delta_Z=%.3fg -- self-test out of range)",
                     st.delta_x, st.delta_y, st.delta_z);
            fault_log_record(FAULT_ADXL355_INIT_FAIL);
        }
    } else {
        ESP_LOGW(TAG, "POST: ADXL355 ABSENT (NaN will be sent for acceleration)");
        fault_log_record(FAULT_ADXL355_INIT_FAIL);
        /* Non-critical: node keeps running, data processing task sends NaN */
    }

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
#endif

    /* ---- SCL3300 ---- */
    ESP_LOGI(TAG, "POST: SCL3300 inclinometer...");
    ret = scl3300_init();
    if (ret == ESP_OK) {
        /* Self-test: read STO register and verify RS bits are normal */
        bool scl_st_passed = false;
        esp_err_t st_err = scl3300_selftest(&scl_st_passed);
        if (st_err == ESP_OK && scl_st_passed) {
            post_scl3300_ok = true;
            ESP_LOGI(TAG, "POST: SCL3300 PASS");
        } else {
            post_scl3300_ok = false;
            ESP_LOGW(TAG, "POST: SCL3300 FAIL  (STO register error or RS bits not normal)");
            fault_log_record(FAULT_SCL3300_INIT_FAIL);
        }
    } else {
        ESP_LOGW(TAG, "POST: SCL3300 ABSENT (NaN will be sent for inclination)");
        fault_log_record(FAULT_SCL3300_INIT_FAIL);
        /* Non-critical: node keeps running, data processing task sends NaN */
    }

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
#endif

    /* ---- POST summary ---- */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========== POST Summary ==========");
    ESP_LOGI(TAG, "  ADXL355  (accelerometer): %s", post_adxl355_ok ? "PASS" : "FAIL / ABSENT");
    ESP_LOGI(TAG, "  SCL3300  (inclinometer):  %s", post_scl3300_ok ? "PASS" : "FAIL / ABSENT");
    ESP_LOGI(TAG, "  ADT7420  (temperature):   %s", post_adt7420_ok ? "PASS" : "FAIL / ABSENT");
    ESP_LOGI(TAG, "  Node continues -- NaN sent for any absent/failed sensor.");
    ESP_LOGI(TAG, "==================================");
    ESP_LOGI(TAG, "");

    /* Always return ESP_OK -- sensor failures are non-critical.
     * The data processing task handles absent sensors with NaN output. */
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

    ESP_LOGI(TAG, "ISR acquisition ready (timer NOT started -- node is IDLE)");
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
                                 const char *cmd_ack, const char *error_msg)
{
    const node_runtime_config_t *cfg = node_config_get();
    mqtt_publish_status_json(
        node_state_str(node_config_get_state()),
        cfg->odr_hz, cfg->range, 200,
        cfg->hpf_corner,
        selftest_ok, seq_ack, cmd_ack, error_msg
    );
}

static void on_mqtt_cmd(const char *topic, const char *payload)
{
    ESP_LOGI("CMD", "Received on [%s]: %s", topic, payload);
    node_state_t state = node_config_get_state();

    /* ---- configure ---- */
    if (strstr(topic, "/cmd/configure")) {
        if (state == NODE_STATE_ERROR) {
            publish_node_status(0, false, NULL, "node in error state, send reset first");
            return;
        }

        int32_t odr_index  = json_get_int(payload, "odr_index",  2);
        int32_t range      = json_get_int(payload, "range",      1);
        int32_t hpf_corner = json_get_int(payload, "hpf_corner", 0);
        int32_t seq        = json_get_int(payload, "seq",        0);

        if (odr_index < 0 || odr_index > 2) {
            publish_node_status((uint32_t)seq, false, NULL, "invalid odr_index");
            return;
        }
        if (range < 1 || range > 3) {
            publish_node_status((uint32_t)seq, false, NULL, "invalid range");
            return;
        }
        if (hpf_corner < 0 || hpf_corner > 6) {
            publish_node_status((uint32_t)seq, false, NULL, "invalid hpf_corner");
            return;
        }

        adxl355_selftest_result_t st_result;
        esp_err_t err = node_config_apply(
            (uint8_t)odr_index, (uint8_t)range,
            (uint8_t)hpf_corner, (uint32_t)seq,
            &st_result
        );

        if (err != ESP_OK) {
            publish_node_status((uint32_t)seq, false, NULL, "register write failed");
            return;
        }

        /* If node was recording before reconfiguration, restart ISR */
        if (state == NODE_STATE_RECORDING) {
            esp_err_t start_err = sensor_acquisition_start();
            if (start_err != ESP_OK) {
                fault_log_record(FAULT_SPI_ERROR);
                node_config_set_error(FAULT_SPI_ERROR);
                publish_node_status((uint32_t)seq, false, NULL, "ISR restart failed");
                return;
            }
            node_config_set_recording();
            ESP_LOGI("CMD", "Reconfiguration complete -- recording resumed");
        }

        publish_node_status((uint32_t)seq, st_result.passed, NULL, NULL);
        return;
    }

    /* ---- control ---- */
    if (strstr(topic, "/cmd/control")) {
        int32_t seq = json_get_int(payload, "seq", 0);

        if (json_str_equals(payload, "cmd", "start")) {
            if (state == NODE_STATE_CONFIGURED) {
                esp_err_t err = sensor_acquisition_start();
                if (err != ESP_OK) {
                    fault_log_record(FAULT_SPI_ERROR);
                    node_config_set_error(FAULT_SPI_ERROR);
                    publish_node_status((uint32_t)seq, false, "start", "ISR start failed");
                    return;
                }
                node_config_set_recording();
                publish_node_status((uint32_t)seq, true, "start", NULL);
            } else if (state == NODE_STATE_RECORDING) {
                ESP_LOGW("CMD", "Already recording");
                publish_node_status((uint32_t)seq, true, "start", NULL);
            } else {
                ESP_LOGW("CMD", "Cannot start from state '%s' -- send configure first",
                         node_state_str(state));
                publish_node_status((uint32_t)seq, false, "start", "must configure before start");
            }
            return;
        }

        if (json_str_equals(payload, "cmd", "stop")) {
            if (state == NODE_STATE_RECORDING) {
                sensor_acquisition_stop();
                node_config_set_configured();
            }
            publish_node_status((uint32_t)seq, true, "stop", NULL);
            return;
        }

        if (json_str_equals(payload, "cmd", "init")) {
            if (state == NODE_STATE_RECORDING) {
                sensor_acquisition_stop();
            }
            adxl355_selftest_result_t st_result;
            esp_err_t err = node_config_apply(2, NODE_RANGE_2G, 0, 0, &st_result);
            if (err != ESP_OK) {
                publish_node_status((uint32_t)seq, false, "init", "init failed");
                return;
            }
            publish_node_status((uint32_t)seq, st_result.passed, "init", NULL);
            return;
        }

        if (json_str_equals(payload, "cmd", "reset")) {
            if (state == NODE_STATE_RECORDING) {
                sensor_acquisition_stop();
            }
            node_config_reset();
            publish_node_status((uint32_t)seq, true, "reset", NULL);
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

    /* Initialise state machine first -- sets state to IDLE, loads defaults. */
    node_config_init();

    init_reboot_counter();

    /* Detect and log the cause of the previous reset at boot time */
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_WDT ||
        reset_reason == ESP_RST_INT_WDT ||
        reset_reason == ESP_RST_TASK_WDT) {
        ESP_LOGW(TAG, "Reset reason: watchdog (reason=%d)", reset_reason);
        fault_log_record(FAULT_WATCHDOG_RESET);
    } else if (reset_reason == ESP_RST_BROWNOUT ||
               reset_reason == ESP_RST_POWERON) {
        ESP_LOGW(TAG, "Reset reason: power loss / brownout (reason=%d)", reset_reason);
        fault_log_record(FAULT_POWER_LOSS);
        fault_log_record(FAULT_POWER_RESTORED);
    }
    print_banner();

    /* Register cmd handler BEFORE mqtt_init so no message can arrive
     * before the handler is wired up. */
    mqtt_set_cmd_handler(on_mqtt_cmd);

    /* Register the IP callback before ethernet_init so it is in place for
     * the very first IP_EVENT_ETH_GOT_IP, including the boot-time one. */
    ethernet_set_got_ip_cb(on_ethernet_got_ip);

    bool network_ok = (init_network() == ESP_OK);
    ESP_LOGI(TAG, "");

    /* MQTT is always started by on_ethernet_got_ip callback, which fires during
     * ethernet_wait_for_ip() above. Never call init_mqtt() here directly --
     * doing so races with the callback and creates two MQTT clients.
     * Instead, wait briefly for the callback to complete then check the result. */
    bool mqtt_ok = false;
    if (network_ok) {
        /* The callback fires from the event task concurrently with
         * ethernet_wait_for_ip() unblocking. Give it up to 2 s to finish. */
        int waited = 0;
        while (!s_mqtt_started && waited < 2000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        if (s_mqtt_started) {
            ESP_LOGI(TAG, "MQTT started by IP callback (waited %d ms)", waited);
            mqtt_ok = true;
        } else {
            /* Callback didn't fire or failed -- start MQTT ourselves */
            ESP_LOGW(TAG, "IP callback did not start MQTT -- starting directly");
            mqtt_ok = (init_mqtt() == ESP_OK);
            if (mqtt_ok) {
                s_mqtt_started = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "Skipping MQTT init - no network (will start when IP obtained)");
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
        ESP_LOGW(TAG, "Sensor initialization had issues -- node will send NaN for failed sensors");
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
            ESP_LOGI(TAG, "SNTP synced after %d ms -- ISR will use UTC timestamps",
                     waited_ms);
        } else {
            ESP_LOGW(TAG, "SNTP sync timeout (%d ms) -- using tick-relative timestamps",
                     waited_ms);
        }
        ESP_LOGI(TAG, "");
    }

    /* Register the fault publish callback only after SNTP has synced (or
     * timed out). This guarantees that every fault timestamp published on
     * wind_turbine/<SERIAL>/faults uses real UTC time, not the tick fallback.
     * Faults that fired earlier in boot are already in the pending buffer
     * and will be flushed with the first data packet once recording starts. */
    if (mqtt_ok) {
        fault_log_set_publish_cb(on_fault_publish);
        ESP_LOGI(TAG, "Fault publish callback registered -- immediate fault reporting active");
        /* Flush any faults that accumulated during boot (reset cause, sensor
         * init failures, etc.) before the callback was registered. These are
         * published now with a valid UTC timestamp so the subscriber receives
         * them regardless of whether the node ever enters RECORDING state. */
        fault_log_flush_pending();
    }

    if (init_acquisition(temp_sensor_available) != ESP_OK) {
        handle_critical_failure("ISR acquisition initialization failed");
    }
    ESP_LOGI(TAG, "");

    if (init_data_processing() != ESP_OK) {
        handle_critical_failure("Data processing task initialization failed");
    }
    ESP_LOGI(TAG, "");

    /* Always build the cmd topic strings so the MQTT_EVENT_CONNECTED handler
     * can re-subscribe on reconnect even if the broker wasn't reachable at boot.
     * mqtt_subscribe_cmd() will also attempt the actual subscription now if
     * already connected -- the call is harmless if not yet connected. */
    mqtt_subscribe_cmd();

    clear_reboot_counter();

    /* Publish initial IDLE status so the Pi knows the node is up */
    if (mqtt_ok) {
        publish_node_status(0, true, NULL, NULL);
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
    ESP_LOGI(TAG, "  SYSTEM RUNNING -- STATE: IDLE");
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