/**
 * @file data_processing_and_mqtt_task.c
 * @brief Data Processing Task - Reads sensor ring buffers, decimates,
 *        batches, and publishes via MQTT.
 *
 * Output rates (fixed, independent of accelerometer ODR):
 *  - Acceleration:  200 Hz  (200 samples per 1-second packet)
 *  - Inclination:    20 Hz  (20 samples batched per packet)
 *  - Temperature:     1 Hz  (1 polled reading per packet)
 *
 * Sensor handling:
 *  - ADXL355: decimated by node_config decim_factor -> 200 Hz output,
 *    batched into node_config batch_size samples (1 second per packet).
 *    If disconnected (watchdog timeout), a NaN-filled packet is emitted
 *    every second so the Pi always sees data arriving.
 *  - SCL3300 (20 Hz): all samples from the ring buffer are batched into
 *    an array of up to 20 readings per packet. If disconnected, NaN is sent.
 *  - ADT7420 (1 Hz): polled directly via I2C. If read fails, NaN is sent.
 *
 * Resilience:
 *  - A disconnected sensor does NOT cause a reboot or halt.
 *  - Each sensor is tracked independently. The node keeps recording the
 *    remaining sensors and sends NaN for the faulty one.
 *  - Faults are logged via fault_log_record() for every event.
 *
 * State awareness:
 *  The task checks node_config_get_state() each loop. It only publishes when
 *  the node is in NODE_STATE_RECORDING. Otherwise it drains and discards ring
 *  buffer contents to prevent backlog.
 */

#include "data_processing_and_mqtt_task.h"
#include "node_config.h"
#include "sensor_task.h"
#include "adt7420.h"
#include "adxl355.h"
#include "scl3300.h"
#include "mqtt.h"
#include "fault_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "DATA_PROC";

static TaskHandle_t  s_task_handle  = NULL;
static volatile bool s_task_running = false;

static volatile uint32_t s_samples_published = 0;
static volatile uint32_t s_packets_sent      = 0;
static volatile uint32_t s_samples_dropped   = 0;
static volatile uint32_t s_temp_read_errors  = 0;

/* Shadow overflow counts — detect new drops since the last check */
static uint32_t s_adxl355_overflow_last = 0;
static uint32_t s_scl3300_overflow_last = 0;
static uint32_t s_adt7420_overflow_last = 0;

/*
 * Per-sensor watchdog: detect physical disconnection by tracking whether the
 * ISR sample counter advances. If a sensor stops producing new samples for
 * longer than the watchdog period it is considered disconnected — log the
 * appropriate dropped-sample fault AND the per-device bus fault.
 *
 * ADXL355 / SCL3300 share the SPI bus but have independent CS lines, so a
 * disconnected SCL3300 must NOT raise FAULT_SPI_ERROR for ADXL355. Each
 * sensor gets its own watchdog and its own SPI fault log.
 * ADT7420 is on I2C; read failures already set temp_valid=false and log
 * FAULT_I2C_ERROR via the existing error path, so its watchdog only needs
 * to track FAULT_ADT7420_DROPPED.
 */
#define SENSOR_WATCHDOG_MS   2000u

static uint32_t s_adxl355_sample_last  = 0;
static uint32_t s_adxl355_watchdog_ms  = 0;
static bool     s_adxl355_disconnected = false;

static uint32_t s_scl3300_sample_last  = 0;
static uint32_t s_scl3300_watchdog_ms  = 0;
static bool     s_scl3300_disconnected = false;

/*
 * Batch buffers sized for the maximum possible batch.
 * ODR=4000Hz with decim=20 -> 200 output samples/sec = 200 per packet.
 * All supported ODR settings produce exactly 200 samples/packet.
 */
#define ACCEL_BATCH_MAX  200
static float    s_accel_x[ACCEL_BATCH_MAX];
static float    s_accel_y[ACCEL_BATCH_MAX];
static float    s_accel_z[ACCEL_BATCH_MAX];
static uint32_t s_accel_ticks[ACCEL_BATCH_MAX];

/*
 * Inclination batch buffer — accumulates all 20 SCL3300 samples per second.
 */
#define INCL_BATCH_MAX   MQTT_INCL_BATCH_SIZE
static float    s_incl_x[INCL_BATCH_MAX];
static float    s_incl_y[INCL_BATCH_MAX];
static float    s_incl_z[INCL_BATCH_MAX];
static uint32_t s_incl_ticks[INCL_BATCH_MAX];
static int      s_incl_batch_count = 0;

#define TEMP_READ_INTERVAL_MS  1000

/*
 * Accel NaN flush: if the ADXL355 is disconnected and no accel batch has been
 * published for this long, emit a NaN-filled packet so the Pi always sees data.
 */
#define ACCEL_NAN_FLUSH_MS     1000u
static uint32_t s_last_accel_publish_ms = 0;

/*
 * SPI sensor reconnection: periodically attempt to re-initialise disconnected
 * SPI sensors.  If the sensor has been physically reconnected, the init
 * sequence will succeed, the ISR will resume producing valid samples, the
 * watchdog will clear the disconnected flag, and real data will flow again.
 */
#define SENSOR_REINIT_INTERVAL_MS   5000u
static uint32_t s_last_reinit_attempt_ms = 0;

/******************************************************************************
 * HELPERS
 *****************************************************************************/

static inline float convert_adxl355_to_g(int32_t raw, float sensitivity_lsb_g)
{
    return (float)raw / sensitivity_lsb_g;
}

static inline float convert_scl3300_to_deg(int16_t raw)
{
    // SCL3300 angle output registers: full-scale +-90 degrees over 16384 LSB
    return (float)raw * (90.0f / 16384.0f);
}

/*
 * Format a wall-clock timestamp into buf (must be >= MQTT_TS_LEN bytes).
 * Back-calculates the exact wall time when the ISR tick was captured.
 * Falls back to "tick:NNNNNNNN" if SNTP has not yet synced.
 */
static void format_ts(char *buf, uint32_t tick)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    if (tv.tv_sec > 1700000000L) {
        uint32_t current_tick = get_tick_count();
        int64_t delta_us = (int64_t)((int32_t)(current_tick - tick)) * 125LL;
        int64_t sample_us = (int64_t)tv.tv_sec * 1000000LL
                            + (int64_t)tv.tv_usec - delta_us;

        time_t sec = (time_t)(sample_us / 1000000LL);
        int    us  = (int)(sample_us % 1000000LL);
        if (us < 0) { us += 1000000; sec -= 1; }

        struct tm tm_info;
        gmtime_r(&sec, &tm_info);
        strftime(buf, MQTT_TS_LEN, "%Y-%m-%dT%H:%M:%S", &tm_info);
        snprintf(buf + 19, MQTT_TS_LEN - 19, ".%06dZ", us);
    } else {
        snprintf(buf, MQTT_TS_LEN, "tick:%08lu", (unsigned long)tick);
    }
}

/* Static packet buffer — keeps large struct off the task stack */
static mqtt_sensor_packet_t s_packet;

static void drain_ring_buffers(void)
{
    adxl355_raw_sample_t a;
    scl3300_raw_sample_t s;
    while (adxl355_data_available()) adxl355_read_sample(&a);
    while (scl3300_data_available()) scl3300_read_sample(&s);
}

/**
 * @brief Drain any pending SCL3300 samples into the incl batch buffers.
 */
static void flush_scl3300_to_batch(void)
{
    scl3300_raw_sample_t scl_sample;
    while (scl3300_data_available()) {
        if (scl3300_read_sample(&scl_sample)) {
            if (s_incl_batch_count < INCL_BATCH_MAX) {
                s_incl_x[s_incl_batch_count]     = convert_scl3300_to_deg(scl_sample.raw_x);
                s_incl_y[s_incl_batch_count]     = convert_scl3300_to_deg(scl_sample.raw_y);
                s_incl_z[s_incl_batch_count]     = convert_scl3300_to_deg(scl_sample.raw_z);
                s_incl_ticks[s_incl_batch_count] = scl_sample.tick;
                s_incl_batch_count++;
            }
        }
    }
}

/**
 * @brief Build and publish one MQTT sensor packet from the current batch buffers.
 *
 * Handles NaN emission for disconnected sensors.
 */
static void publish_packet(int accel_count, bool accel_valid,
                           int incl_count,  bool incl_valid,
                           bool temp_valid_arg, float current_temp,
                           const char *current_temp_ts,
                           uint32_t odr_hz)
{
    mqtt_sensor_packet_t *packet = &s_packet;
    memset(packet, 0, sizeof(*packet));

    /* ---- Acceleration ---- */
    packet->accel_valid = accel_valid;
    packet->accel_count = accel_count;
    if (accel_valid && accel_count > 0) {
        for (int i = 0; i < accel_count; i++) {
            packet->accel[i].x = s_accel_x[i];
            packet->accel[i].y = s_accel_y[i];
            packet->accel[i].z = s_accel_z[i];
            format_ts(packet->accel[i].ts, s_accel_ticks[i]);
        }
    }

    /* ---- Inclination (batched) ---- */
    packet->incl_valid = incl_valid;
    packet->incl_count = incl_count;
    if (incl_valid && incl_count > 0) {
        for (int i = 0; i < incl_count && i < MQTT_INCL_BATCH_SIZE; i++) {
            packet->incl[i].x = s_incl_x[i];
            packet->incl[i].y = s_incl_y[i];
            packet->incl[i].z = s_incl_z[i];
            format_ts(packet->incl[i].ts, s_incl_ticks[i]);
        }
    }

    /* ---- Temperature ---- */
    packet->has_temp   = true;
    packet->temp_valid = temp_valid_arg;
    if (temp_valid_arg) {
        packet->temperature = current_temp;
        memcpy(packet->temp_ts, current_temp_ts, MQTT_TS_LEN);
    }

    /* ---- Publish ---- */
    esp_err_t pub_ret = mqtt_publish_sensor_data(packet);
    if (pub_ret == ESP_OK) {
        s_samples_published += (uint32_t)(accel_valid ? accel_count : 0);
        s_packets_sent++;
        s_last_accel_publish_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGD(TAG, "pkt#%lu accel=%d/%s incl=%d/%s temp=%s odr=%luHz",
                 (unsigned long)s_packets_sent,
                 accel_count, accel_valid ? "ok" : "NaN",
                 incl_count,  incl_valid  ? "ok" : "NaN",
                 temp_valid_arg ? "ok" : "NaN",
                 (unsigned long)odr_hz);
    } else {
        s_samples_dropped += (uint32_t)(accel_valid ? accel_count : 0);
        static uint32_t s_last_drop_ms = 0;
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
        if ((t - s_last_drop_ms) >= 1000) {
            s_last_drop_ms = t;
            ESP_LOGW(TAG, "MQTT not ready — dropped %lu samples total",
                     (unsigned long)s_samples_dropped);
        }
    }
}

/******************************************************************************
 * MAIN TASK
 *****************************************************************************/

static void data_processing_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data processing task started");

    adxl355_raw_sample_t adxl_sample;

    int      decim_count       = 0;
    int64_t  sum_x             = 0;
    int64_t  sum_y             = 0;
    int64_t  sum_z             = 0;
    uint32_t decim_first_tick  = 0;
    int      accel_batch_count = 0;

    uint32_t last_temp_read_ms = (uint32_t)(esp_timer_get_time() / 1000)
                                 - TEMP_READ_INTERVAL_MS;
    float current_temp  = 0.0f;
    bool  temp_valid    = false;
    char  current_temp_ts[MQTT_TS_LEN] = {0};

    bool  incl_ever_received = false;

    uint32_t last_logged_odr = 0;

    s_last_accel_publish_ms = (uint32_t)(esp_timer_get_time() / 1000);

    while (s_task_running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* ------------------------------------------------------------------ */
        /* Only run the data pipeline in RECORDING state                      */
        /* ------------------------------------------------------------------ */
        node_state_t state = node_config_get_state();
        if (state != NODE_STATE_RECORDING) {
            drain_ring_buffers();
            decim_count       = 0;
            sum_x = sum_y = sum_z = 0;
            accel_batch_count  = 0;
            s_incl_batch_count = 0;
            /* Reset watchdog timers so stale counters don't fire on next start */
            s_adxl355_sample_last  = adxl355_get_sample_count();
            s_scl3300_sample_last  = scl3300_get_sample_count();
            s_adxl355_watchdog_ms  = 0;
            s_scl3300_watchdog_ms  = 0;
            s_adxl355_disconnected = false;
            s_scl3300_disconnected = false;
            incl_ever_received     = false;
            s_last_accel_publish_ms = now_ms;
            vTaskDelay(pdMS_TO_TICKS(PROCESSING_INTERVAL_MS));
            continue;
        }

        /* Snapshot runtime config once per iteration */
        const node_runtime_config_t *cfg = node_config_get();
        uint32_t decim_factor      = cfg->decim_factor;
        uint32_t batch_size        = cfg->batch_size;
        float    sensitivity_lsb_g = cfg->sensitivity_lsb_g;
        uint32_t odr_hz            = cfg->odr_hz;

        /* ------------------------------------------------------------------ */
        /* Sensor health: ring-buffer overflows + per-device disconnect        */
        /* watchdog.                                                           */
        /* ------------------------------------------------------------------ */
        {
            uint32_t adxl_ov  = adxl355_get_overflow_count();
            uint32_t scl_ov   = scl3300_get_overflow_count();
            uint32_t adt_ov   = adt7420_get_overflow_count();
            uint32_t adxl_cnt = adxl355_get_sample_count();
            uint32_t scl_cnt  = scl3300_get_sample_count();

            /* --- Overflow faults --- */
            if (adxl_ov != s_adxl355_overflow_last) {
                fault_log_record(FAULT_ADXL355_DROPPED);
                s_adxl355_overflow_last = adxl_ov;
            }
            if (scl_ov != s_scl3300_overflow_last) {
                fault_log_record(FAULT_SCL3300_DROPPED);
                s_scl3300_overflow_last = scl_ov;
            }
            if (adt_ov != s_adt7420_overflow_last) {
                fault_log_record(FAULT_ADT7420_DROPPED);
                s_adt7420_overflow_last = adt_ov;
            }

            /* --- ADXL355 disconnect watchdog --- */
            if (adxl_cnt != s_adxl355_sample_last) {
                s_adxl355_sample_last  = adxl_cnt;
                s_adxl355_watchdog_ms  = 0;
                if (s_adxl355_disconnected) {
                    ESP_LOGI(TAG, "ADXL355 reconnected");
                    s_adxl355_disconnected = false;
                }
            } else {
                s_adxl355_watchdog_ms += PROCESSING_INTERVAL_MS;
                if (s_adxl355_watchdog_ms >= SENSOR_WATCHDOG_MS && !s_adxl355_disconnected) {
                    ESP_LOGW(TAG, "ADXL355 stalled — sensor may be disconnected");
                    fault_log_record(FAULT_ADXL355_DROPPED);
                    fault_log_record(FAULT_SPI_ERROR);
                    s_adxl355_disconnected = true;
                }
            }

            /* --- SCL3300 disconnect watchdog --- */
            if (scl_cnt != s_scl3300_sample_last) {
                s_scl3300_sample_last  = scl_cnt;
                s_scl3300_watchdog_ms  = 0;
                if (s_scl3300_disconnected) {
                    ESP_LOGI(TAG, "SCL3300 reconnected");
                    s_scl3300_disconnected = false;
                }
            } else {
                s_scl3300_watchdog_ms += PROCESSING_INTERVAL_MS;
                if (s_scl3300_watchdog_ms >= SENSOR_WATCHDOG_MS && !s_scl3300_disconnected) {
                    ESP_LOGW(TAG, "SCL3300 stalled — sensor may be disconnected");
                    fault_log_record(FAULT_SCL3300_DROPPED);
                    fault_log_record(FAULT_SPI_ERROR);
                    s_scl3300_disconnected = true;
                }
            }
        }

        if (odr_hz != last_logged_odr) {
            ESP_LOGI(TAG, "Recording: ODR=%lu Hz decim=%lu batch=%lu sens=%.0f LSB/g",
                     (unsigned long)odr_hz, (unsigned long)decim_factor,
                     (unsigned long)batch_size, sensitivity_lsb_g);
            last_logged_odr = odr_hz;
        }

        /* ------------------------------------------------------------------ */
        /* SPI sensor reconnection: periodically attempt to reinit any        */
        /* disconnected SPI sensor so it can recover after hot-plug.          */
        /* ------------------------------------------------------------------ */
        if ((s_adxl355_disconnected || s_scl3300_disconnected) &&
            (now_ms - s_last_reinit_attempt_ms) >= SENSOR_REINIT_INTERVAL_MS) {

            s_last_reinit_attempt_ms = now_ms;

            /*
             * Inhibit ALL SPI sensor reads in the ISR for the duration of any
             * reinit attempt. Both ADXL355 and SCL3300 share the same SPI bus,
             * and the ISR's polling transmits from ISR context would collide
             * with the task-context init sequence, corrupting both.
             */
            adxl355_isr_set_inhibit(true);
            scl3300_isr_set_inhibit(true);

            if (s_adxl355_disconnected) {
                ESP_LOGI(TAG, "Attempting ADXL355 reinit...");
                esp_err_t err;
                err = adxl355_write_reg_pub(ADXL355_REG_RESET, 0x52);
                if (err == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    uint8_t devid = 0;
                    err = adxl355_read_reg_pub(ADXL355_REG_DEVID_AD, &devid, 1);
                    if (err == ESP_OK && devid == ADXL355_DEVID_AD_EXPECTED) {
                        const adxl355_odr_config_t *odr_cfg = node_config_get_odr(cfg->odr_index);
                        adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, ADXL355_POWER_STANDBY_BIT);
                        vTaskDelay(pdMS_TO_TICKS(2));
                        if (odr_cfg) {
                            adxl355_write_reg_pub(ADXL355_REG_FILTER, odr_cfg->filter_reg);
                        }
                        adxl355_write_reg_pub(ADXL355_REG_INT_MAP, ADXL355_INT_RDY_EN1);
                        adxl355_set_range(cfg->range);
                        adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, 0x00);
                        ESP_LOGI(TAG, "ADXL355 reinit succeeded — waiting for samples");
                    } else {
                        ESP_LOGD(TAG, "ADXL355 reinit: sensor not responding yet");
                    }
                }
            }

            if (s_scl3300_disconnected) {
                ESP_LOGI(TAG, "Attempting SCL3300 reinit...");
                esp_err_t err = scl3300_init();
                if (err == ESP_OK) {
                    scl3300_reset_isr_pipeline();
                    ESP_LOGI(TAG, "SCL3300 reinit succeeded — waiting for samples");
                } else {
                    ESP_LOGD(TAG, "SCL3300 reinit: sensor not responding yet");
                }
            }

            /* Re-enable ISR reads for both sensors */
            adxl355_isr_set_inhibit(false);
            scl3300_isr_set_inhibit(false);
        }

        /* ------------------------------------------------------------------ */
        /* Poll temperature (I2C, task context only)                          */
        /* ------------------------------------------------------------------ */
        if ((now_ms - last_temp_read_ms) >= TEMP_READ_INTERVAL_MS) {
            last_temp_read_ms = now_ms;
            float tc = 0.0f;
            esp_err_t err = adt7420_read_temperature(&tc);
            if (err == ESP_OK) {
                current_temp = tc;
                temp_valid   = true;
                s_temp_read_errors = 0;
                format_ts(current_temp_ts, get_tick_count());
            } else {
                temp_valid = false;
                s_temp_read_errors++;
                ESP_LOGW(TAG, "Temp read failed: %s (#%lu)",
                         esp_err_to_name(err), (unsigned long)s_temp_read_errors);
                if (s_temp_read_errors == 1 || (s_temp_read_errors % 10) == 0) {
                    fault_log_record(FAULT_ADT7420_DROPPED);
                    fault_log_record(FAULT_I2C_ERROR);
                }
            }
        }

        /* ------------------------------------------------------------------ */
        /* Drain SCL3300 ring buffer — batch all samples (up to 20/sec)       */
        /* ------------------------------------------------------------------ */
        flush_scl3300_to_batch();

        if (s_incl_batch_count > 0) {
            incl_ever_received = true;
        } else if (!incl_ever_received && !s_scl3300_disconnected) {
            static bool s_incl_warned = false;
            if (!s_incl_warned) {
                ESP_LOGW(TAG, "No inclinometer data received yet (will retry)");
                s_incl_warned = true;
            }
        }

        /* ------------------------------------------------------------------ */
        /* Drain ADXL355 ring buffer — decimate and batch                     */
        /* ------------------------------------------------------------------ */
        while (adxl355_data_available() && s_task_running) {

            /* Break cleanly if state changes mid-drain */
            if (node_config_get_state() != NODE_STATE_RECORDING) {
                decim_count = 0;
                sum_x = sum_y = sum_z = 0;
                accel_batch_count  = 0;
                s_incl_batch_count = 0;
                break;
            }

            if (!adxl355_read_sample(&adxl_sample)) break;

            if (decim_count == 0) {
                decim_first_tick = adxl_sample.tick;
            }

            sum_x += (int64_t)adxl_sample.raw_x;
            sum_y += (int64_t)adxl_sample.raw_y;
            sum_z += (int64_t)adxl_sample.raw_z;
            decim_count++;

            if ((uint32_t)decim_count >= decim_factor) {
                int32_t avg_x = (int32_t)(sum_x / (int64_t)decim_factor);
                int32_t avg_y = (int32_t)(sum_y / (int64_t)decim_factor);
                int32_t avg_z = (int32_t)(sum_z / (int64_t)decim_factor);

                s_accel_x[accel_batch_count] = convert_adxl355_to_g(avg_x, sensitivity_lsb_g);
                s_accel_y[accel_batch_count] = convert_adxl355_to_g(avg_y, sensitivity_lsb_g);
                s_accel_z[accel_batch_count] = convert_adxl355_to_g(avg_z, sensitivity_lsb_g);
                s_accel_ticks[accel_batch_count] = decim_first_tick;
                accel_batch_count++;

                decim_count = 0;
                sum_x = sum_y = sum_z = 0;

                if ((uint32_t)accel_batch_count >= batch_size) {
                    /* Flush any late SCL3300 samples before publishing */
                    flush_scl3300_to_batch();

                    bool incl_valid_now = (s_incl_batch_count > 0) && !s_scl3300_disconnected;

                    publish_packet(accel_batch_count, true,
                                   s_incl_batch_count, incl_valid_now,
                                   temp_valid, current_temp, current_temp_ts,
                                   odr_hz);

                    accel_batch_count  = 0;
                    s_incl_batch_count = 0;
                }
            }
        }

        /* ------------------------------------------------------------------ */
        /* ADXL355 disconnected: periodically flush a NaN packet so the Pi    */
        /* always receives data at ~1 Hz even when accel is missing.          */
        /* ------------------------------------------------------------------ */
        if (s_adxl355_disconnected &&
            (now_ms - s_last_accel_publish_ms) >= ACCEL_NAN_FLUSH_MS) {

            /* Drain any incl samples that arrived */
            flush_scl3300_to_batch();

            bool incl_valid_now = (s_incl_batch_count > 0) && !s_scl3300_disconnected;

            publish_packet(0, false,                             /* accel = NaN */
                           s_incl_batch_count, incl_valid_now,
                           temp_valid, current_temp, current_temp_ts,
                           odr_hz);

            s_incl_batch_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(PROCESSING_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Data processing task stopped");
    vTaskDelete(NULL);
}

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

esp_err_t data_processing_and_mqtt_task_init(void)
{
    ESP_LOGI(TAG, "Initializing data processing task...");

    s_samples_published = 0;
    s_packets_sent      = 0;
    s_samples_dropped   = 0;
    s_temp_read_errors  = 0;

    s_task_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        data_processing_task,
        "data_proc",
        DATA_PROCESSING_TASK_STACK_SIZE,
        NULL,
        DATA_PROCESSING_TASK_PRIORITY,
        &s_task_handle,
        DATA_PROCESSING_TASK_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Task started (priority=%d, core=%d)",
             DATA_PROCESSING_TASK_PRIORITY, DATA_PROCESSING_TASK_CORE);

    return ESP_OK;
}

esp_err_t data_processing_and_mqtt_task_stop(void)
{
    ESP_LOGI(TAG, "Stopping data processing task...");
    s_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    s_task_handle = NULL;
    ESP_LOGI(TAG, "Task stopped");
    return ESP_OK;
}

void data_processing_and_mqtt_task_get_stats(uint32_t *samples_published,
                                              uint32_t *packets_sent,
                                              uint32_t *samples_dropped)
{
    if (samples_published) *samples_published = s_samples_published;
    if (packets_sent)      *packets_sent      = s_packets_sent;
    if (samples_dropped)   *samples_dropped   = s_samples_dropped;
}
