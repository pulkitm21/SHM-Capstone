/**
 * @file fault_log.c
 * @brief Fault logging system implementation
 *
 * Maintains a small fixed-size ring buffer of pending uint8_t fault codes.
 * No heap allocation. No strings stored on the ESP32 side — just integers.
 * The Raspberry Pi subscriber holds the lookup table for human-readable text.
 */

#include "fault_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/time.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "FAULT_LOG";

/******************************************************************************
 * INTERNAL STATE
 *****************************************************************************/

static uint8_t           s_pending[FAULT_LOG_MAX_PENDING];
static int               s_pending_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

/* Registered callback for immediate per-fault MQTT publishing (may be NULL) */
static fault_publish_cb_t s_publish_cb = NULL;

/* Scratch buffers for per-fault JSON and topic — sized conservatively:
 *   topic: "wind_turbine/" (13) + serial (32) + "/faults" (7) + '\0' = 53 → 80
 *   json:  {"ts":"2025-01-15T12:34:56.000000Z","f":18} = ~50 chars → 96      */
#define FAULT_JSON_BUF_SIZE    96
#define FAULT_TOPIC_BUF_SIZE   80
#define FAULT_TS_BUF_SIZE      40

/* Lazy-init the mutex on first use so no explicit init call is needed */
static SemaphoreHandle_t get_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    return s_mutex;
}

/******************************************************************************
 * INTERNAL HELPERS
 *****************************************************************************/

/*
 * Format a UTC ISO-8601 timestamp into buf (>= FAULT_TS_BUF_SIZE bytes).
 * Uses gettimeofday() directly — we want the wall time of the fault event.
 * Mirrors the 1700000000L validity threshold used in sntp_sync.c.
 * Falls back to "tick:NNNNNNNNNN" (µs from esp_timer_get_time) if SNTP
 * has not yet synced.
 */
static void fault_format_ts(char *buf, size_t buf_size)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    if (tv.tv_sec > 1700000000L) {
        struct tm tm_info;
        gmtime_r(&tv.tv_sec, &tm_info);
        char date_buf[28];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
        snprintf(buf, buf_size, "%s.%06ldZ", date_buf, (long)tv.tv_usec);
    } else {
        /* SNTP not yet synced — µs tick gives a monotonic fallback reference */
        snprintf(buf, buf_size, "tick:%08llu",
                 (unsigned long long)(esp_timer_get_time()));
    }
}

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

void fault_log_record(uint8_t fault_code)
{
    SemaphoreHandle_t m = get_mutex();
    if (m == NULL) {
        return;
    }

    if (xSemaphoreTake(m, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_pending_count < FAULT_LOG_MAX_PENDING) {
            s_pending[s_pending_count++] = fault_code;
        } else {
            /* Buffer full: shift oldest out, add newest at end */
            memmove(&s_pending[0], &s_pending[1], FAULT_LOG_MAX_PENDING - 1);
            s_pending[FAULT_LOG_MAX_PENDING - 1] = fault_code;
            ESP_LOGW(TAG, "Fault buffer full — oldest fault overwritten");
        }
        ESP_LOGI(TAG, "Fault recorded: code=%d", fault_code);
        xSemaphoreGive(m);
    }

    /* -------------------------------------------------------------------
     * Immediately publish an individual fault packet via the registered
     * callback, regardless of sensor state (idle or recording).
     *
     * Topic:  wind_turbine/<SERIAL>/faults
     * Format: {"ts":"2025-01-15T12:34:56.000000Z","f":7}
     *
     * The callback is registered from main.c after mqtt_init() — this
     * keeps fault_log independent of mqtt (no circular dependency).
     * If the callback is NULL (MQTT not yet up) the fault is still safe
     * in the pending buffer and will appear in the next data packet.
     * ------------------------------------------------------------------- */
    fault_publish_cb_t cb = s_publish_cb;   /* snapshot — avoids race on deinit */
    if (cb != NULL) {
        char ts_buf[FAULT_TS_BUF_SIZE];
        char json_buf[FAULT_JSON_BUF_SIZE];

        fault_format_ts(ts_buf, sizeof(ts_buf));

        snprintf(json_buf, sizeof(json_buf),
                 "{\"ts\":\"%s\",\"f\":%d}", ts_buf, fault_code);

        cb(json_buf, (int)strlen(json_buf));

        ESP_LOGI(TAG, "Fault dispatched immediately: payload=%s", json_buf);
    } else {
        ESP_LOGD(TAG, "No publish callback — fault %d in pending buffer only",
                 fault_code);
    }
}

void fault_log_set_publish_cb(fault_publish_cb_t cb)
{
    s_publish_cb = cb;
    ESP_LOGI(TAG, "Fault publish callback %s", cb ? "registered" : "cleared");
}

void fault_log_flush_pending(void)
{
    fault_publish_cb_t cb = s_publish_cb;
    if (cb == NULL) {
        return;
    }

    SemaphoreHandle_t m = get_mutex();
    if (m == NULL) return;

    if (xSemaphoreTake(m, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }

    int count = s_pending_count;
    uint8_t snapshot[FAULT_LOG_MAX_PENDING];
    memcpy(snapshot, s_pending, count);
    s_pending_count = 0;   /* clear now so the buffer is free while we publish */

    xSemaphoreGive(m);

    if (count == 0) {
        return;
    }

    ESP_LOGI(TAG, "Flushing %d buffered fault(s) accumulated before callback was ready", count);

    char ts_buf[FAULT_TS_BUF_SIZE];
    char json_buf[FAULT_JSON_BUF_SIZE];

    fault_format_ts(ts_buf, sizeof(ts_buf));  /* one shared timestamp for the flush batch */

    for (int i = 0; i < count; i++) {
        snprintf(json_buf, sizeof(json_buf),
                 "{\"ts\":\"%s\",\"f\":%d}", ts_buf, snapshot[i]);
        cb(json_buf, (int)strlen(json_buf));
        ESP_LOGI(TAG, "Flushed buffered fault: code=%d payload=%s", snapshot[i], json_buf);
    }
}

bool fault_log_has_pending(void)
{
    SemaphoreHandle_t m = get_mutex();
    if (m == NULL) return false;

    bool has = false;
    if (xSemaphoreTake(m, pdMS_TO_TICKS(10)) == pdTRUE) {
        has = (s_pending_count > 0);
        xSemaphoreGive(m);
    }
    return has;
}

int fault_log_append_to_json(char *buf, int buf_size, int offset)
{
    if (buf == NULL || buf_size <= 0 || offset >= buf_size) {
        return offset;
    }

    SemaphoreHandle_t m = get_mutex();
    if (m == NULL) return offset;

    if (xSemaphoreTake(m, pdMS_TO_TICKS(10)) != pdTRUE) {
        return offset;
    }

    if (s_pending_count == 0) {
        xSemaphoreGive(m);
        return offset;
    }

    /* Write:  ,"f":[1,7,12]  */
    offset += snprintf(buf + offset, buf_size - offset, ",\"f\":[");

    for (int i = 0; i < s_pending_count; i++) {
        if (i > 0) {
            offset += snprintf(buf + offset, buf_size - offset, ",");
        }
        offset += snprintf(buf + offset, buf_size - offset, "%d", s_pending[i]);

        if (offset >= buf_size - 4) {
            /* Safety: stop writing if we're running out of room */
            ESP_LOGW(TAG, "JSON buffer nearly full while writing fault codes");
            break;
        }
    }

    offset += snprintf(buf + offset, buf_size - offset, "]");

    /* Clear the pending list now that they have been written into the packet */
    s_pending_count = 0;


    xSemaphoreGive(m);
    return offset;
}
