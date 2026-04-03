/**
 * @file sntp_sync.c
 * @brief SNTP clock synchronisation implementation.
 *
 * Uses ESP-IDF's built-in SNTP client (lwIP SNTP, no extra component needed).
 * On successful sync the system clock is set via settimeofday() by the SNTP
 * stack internally, making gettimeofday() return correct UTC time throughout
 * the firmware — including in format_ts() in data_processing_and_mqtt_task.c.
 *
 * Sync mode is SMOOTH (slew-only, never step) so the clock advances
 * monotonically and tick-to-wall back-calculations remain valid across syncs.
 */

#include "sntp_sync.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <sys/time.h>

static const char *TAG = "SNTP";

/* Threshold: any tv_sec above this is a valid post-2024 UTC timestamp. */
#define VALID_UTC_THRESHOLD  1700000000L   /* 2023-11-15 */

/******************************************************************************
 * Sync notification callback
 *****************************************************************************/

static void sntp_sync_cb(struct timeval *tv)
{
    struct tm tm_info;
    gmtime_r(&tv->tv_sec, &tm_info);
    ESP_LOGI(TAG, "SNTP sync complete: %04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
}

/******************************************************************************
 * Public API
 *****************************************************************************/

esp_err_t sntp_sync_init(void)
{
    ESP_LOGI(TAG, "Initialising SNTP client -> %s", SNTP_SERVER_HOSTNAME);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER_HOSTNAME);

    /* SMOOTH mode slews the clock gradually — never steps it.
     * This keeps gettimeofday() monotonic so tick back-calculations
     * in format_ts() are never invalidated by a sudden time jump. */
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);

    /* Re-sync every SNTP_SYNC_POLL_INTERVAL_MS milliseconds. */
    sntp_set_sync_interval(SNTP_SYNC_POLL_INTERVAL_MS);

    sntp_set_time_sync_notification_cb(sntp_sync_cb);

    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP client started (poll interval: %d s)",
             SNTP_SYNC_POLL_INTERVAL_MS / 1000);
    return ESP_OK;
}

bool sntp_sync_is_valid(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec > VALID_UTC_THRESHOLD);
}
