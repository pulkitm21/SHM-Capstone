/**
 * @file data_processing_and_mqtt_task.c
 * @brief Data Processing Task - Reads sensor ring buffers, decimates,
 *        batches, and publishes via MQTT.
 *
 * Sensor handling:
 *  - ADXL355: decimated by node_config decim_factor -> 200 Hz output,
 *    batched into node_config batch_size samples (1 second per packet).
 *    Both values are read from the runtime config each loop so they update
 *    automatically after reconfiguration without restarting this task.
 *  - SCL3300 (20 Hz): sticky — last valid angle held until overwritten.
 *    Conversion: raw * 0.0055f deg/LSB (fixed, not reconfigurable).
 *  - ADT7420 (1 Hz): polled directly via I2C. Sticky value between polls.
 *
 * State awareness:
 *  The task checks node_config_get_state() each loop. It only publishes when
 *  the node is in NODE_STATE_RECORDING. Otherwise it drains and discards ring
 *  buffer contents to prevent backlog, and resets the decimation accumulators
 *  so a clean batch starts when recording resumes.
 *
 * Timestamp:
 *  packet.timestamp = TICKS_TO_US(first_tick) — tick of the first raw sample
 *  in the batch. Converted to microseconds. The Pi side uses this as the
 *  absolute time of the first sample in each packet.
 */ //test

#include "data_processing_and_mqtt_task.h"
#include "node_config.h"
#include "sensor_task.h"
#include "adt7420.h"
#include "mqtt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "DATA_PROC";

static TaskHandle_t  s_task_handle  = NULL;
static volatile bool s_task_running = false;

static volatile uint32_t s_samples_published = 0;
static volatile uint32_t s_packets_sent      = 0;
static volatile uint32_t s_samples_dropped   = 0;
static volatile uint32_t s_temp_read_errors  = 0;

/*
 * Batch buffers sized for the maximum possible batch.
 * ODR=4000Hz with decim=20 → 200 output samples/sec = 200 per packet.
 * All supported ODR settings produce exactly 200 samples/packet.
 */
#define ACCEL_BATCH_MAX  200
static float    s_accel_x[ACCEL_BATCH_MAX];
static float    s_accel_y[ACCEL_BATCH_MAX];
static float    s_accel_z[ACCEL_BATCH_MAX];
static uint32_t s_accel_ticks[ACCEL_BATCH_MAX];

#define TEMP_READ_INTERVAL_MS  1000

/******************************************************************************
 * HELPERS
 *****************************************************************************/

static inline float convert_adxl355_to_g(int32_t raw, float sensitivity_lsb_g)
{
    return (float)raw / sensitivity_lsb_g;
}

static inline float convert_scl3300_to_deg(int16_t raw)
{
    /* SCL3300 acceleration registers: 0.0055 deg/LSB */
    return (float)raw * 0.0055f;
}

static void drain_ring_buffers(void)
{
    adxl355_raw_sample_t a;
    scl3300_raw_sample_t s;
    while (adxl355_data_available()) adxl355_read_sample(&a);
    while (scl3300_data_available()) scl3300_read_sample(&s);
}

/******************************************************************************
 * MAIN TASK
 *****************************************************************************/

static void data_processing_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data processing task started");

    adxl355_raw_sample_t adxl_sample;
    scl3300_raw_sample_t scl_sample;

    int      decim_count       = 0;
    int64_t  sum_x             = 0;
    int64_t  sum_y             = 0;
    int64_t  sum_z             = 0;
    uint32_t decim_first_tick  = 0;
    int      accel_batch_count = 0;
    uint32_t first_tick        = 0;   /* tick of first raw sample in this packet */

    uint32_t last_temp_read_ms = (uint32_t)(esp_timer_get_time() / 1000)
                                 - TEMP_READ_INTERVAL_MS;
    float current_temp  = 0.0f;
    bool  temp_valid    = false;

    float current_incl_x = 0.0f;
    float current_incl_y = 0.0f;
    float current_incl_z = 0.0f;
    bool  incl_valid     = false;

    uint32_t last_logged_odr = 0;

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
            accel_batch_count = 0;
            vTaskDelay(pdMS_TO_TICKS(PROCESSING_INTERVAL_MS));
            continue;
        }

        /* Snapshot runtime config once per iteration */
        const node_runtime_config_t *cfg = node_config_get();
        uint32_t decim_factor      = cfg->decim_factor;
        uint32_t batch_size        = cfg->batch_size;
        float    sensitivity_lsb_g = cfg->sensitivity_lsb_g;
        uint32_t odr_hz            = cfg->odr_hz;

        if (odr_hz != last_logged_odr) {
            ESP_LOGI(TAG, "Recording: ODR=%lu Hz decim=%lu batch=%lu sens=%.0f LSB/g",
                     (unsigned long)odr_hz, (unsigned long)decim_factor,
                     (unsigned long)batch_size, sensitivity_lsb_g);
            last_logged_odr = odr_hz;
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
            } else {
                temp_valid = false;
                s_temp_read_errors++;
                ESP_LOGW(TAG, "Temp read failed: %s (#%lu)",
                         esp_err_to_name(err), (unsigned long)s_temp_read_errors);
            }
        }

        /* ------------------------------------------------------------------ */
        /* Drain SCL3300 ring buffer (sticky)                                 */
        /* ------------------------------------------------------------------ */
        bool new_incl_this_cycle = false;
        while (scl3300_data_available()) {
            if (scl3300_read_sample(&scl_sample)) {
                current_incl_x      = convert_scl3300_to_deg(scl_sample.raw_x);
                current_incl_y      = convert_scl3300_to_deg(scl_sample.raw_y);
                current_incl_z      = convert_scl3300_to_deg(scl_sample.raw_z);
                incl_valid          = true;
                new_incl_this_cycle = true;
            }
        }

        if (!new_incl_this_cycle && !incl_valid) {
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
                accel_batch_count = 0;
                break;
            }

            if (!adxl355_read_sample(&adxl_sample)) break;

            /* Capture tick of the very first raw sample in this packet */
            if (accel_batch_count == 0 && decim_count == 0) {
                first_tick = adxl_sample.tick;
            }

            /* Capture tick of the first raw sample in this decimation window */
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
                    while (scl3300_data_available()) {
                        if (scl3300_read_sample(&scl_sample)) {
                            current_incl_x = convert_scl3300_to_deg(scl_sample.raw_x);
                            current_incl_y = convert_scl3300_to_deg(scl_sample.raw_y);
                            current_incl_z = convert_scl3300_to_deg(scl_sample.raw_z);
                            incl_valid     = true;
                            new_incl_this_cycle = true;
                        }
                    }

                    /* Build packet */
                    mqtt_sensor_packet_t packet = {0};
                    packet.timestamp   = TICKS_TO_US(first_tick);
                    packet.accel_count = accel_batch_count;

                    for (int i = 0; i < accel_batch_count; i++) {
                        packet.accel[i].x = s_accel_x[i];
                        packet.accel[i].y = s_accel_y[i];
                        packet.accel[i].z = s_accel_z[i];
                    }

                    packet.has_angle   = true;
                    packet.angle_valid = incl_valid;
                    if (incl_valid) {
                        packet.angle_x = current_incl_x;
                        packet.angle_y = current_incl_y;
                        packet.angle_z = current_incl_z;
                    }

                    packet.has_temp   = true;
                    packet.temp_valid = temp_valid;
                    if (temp_valid) {
                        packet.temperature = current_temp;
                    }

                    esp_err_t pub_ret = mqtt_publish_sensor_data(&packet);
                    if (pub_ret == ESP_OK) {
                        s_samples_published += (uint32_t)accel_batch_count;
                        s_packets_sent++;
                        ESP_LOGD(TAG, "pkt#%lu ts=%luus incl=%s temp=%s odr=%luHz",
                                 (unsigned long)s_packets_sent,
                                 (unsigned long)packet.timestamp,
                                 packet.angle_valid ? "ok" : "null",
                                 packet.temp_valid  ? "ok" : "null",
                                 (unsigned long)odr_hz);
                    } else {
                        s_samples_dropped += (uint32_t)accel_batch_count;
                        static uint32_t s_last_drop_ms = 0;
                        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
                        if ((t - s_last_drop_ms) >= 1000) {
                            s_last_drop_ms = t;
                            ESP_LOGW(TAG, "MQTT not ready — dropped %lu samples total",
                                     (unsigned long)s_samples_dropped);
                        }
                    }

                    accel_batch_count = 0;
                }
            }
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
