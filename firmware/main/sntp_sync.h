/**
 * @file sntp_sync.h
 * @brief SNTP clock synchronisation — sets the system wall clock from the
 *        Raspberry Pi's NTP server so that gettimeofday() returns UTC time.
 *
 * Usage:
 *   1. Call sntp_sync_init() once, after ethernet is up and mDNS is ready
 *      (i.e. after mqtt_mdns_init() has been called).
 *   2. Call sntp_sync_is_valid() to check whether the first sync has occurred.
 *      format_ts() in data_processing_and_mqtt_task.c uses gettimeofday()
 *      directly and falls back to tick-relative strings until this returns true.
 */

#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How often the SNTP client re-syncs (milliseconds). 30 s keeps drift < 1.5 ms. */
#define SNTP_SYNC_POLL_INTERVAL_MS  30000

/** SNTP server — same hostname used for MQTT, resolved via mDNS. */
#define SNTP_SERVER_HOSTNAME        "raspberrypi.local"

/**
 * @brief Initialise and start the SNTP client.
 *
 * Must be called AFTER mqtt_mdns_init() so that mDNS can resolve
 * SNTP_SERVER_HOSTNAME.  On first successful sync the ESP-IDF SNTP stack
 * calls settimeofday() internally, which makes gettimeofday() return UTC.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sntp_sync_init(void);

/**
 * @brief Returns true once gettimeofday() is returning a valid UTC time
 *        (i.e. at least one SNTP sync has completed and tv_sec > 2024).
 */
bool sntp_sync_is_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* SNTP_SYNC_H */
