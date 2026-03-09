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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "FAULT_LOG";

/******************************************************************************
 * INTERNAL STATE
 *****************************************************************************/

static uint8_t          s_pending[FAULT_LOG_MAX_PENDING];
static int              s_pending_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

/* Lazy-init the mutex on first use so no explicit init call is needed */
static SemaphoreHandle_t get_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    return s_mutex;
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
    //s_pending_count = 0; // TODO: uncomment this when done testing

    xSemaphoreGive(m);
    return offset;
}
