/**
 * @file sntp_sync.h
 * @brief SNTP clock synchronisation with tick-anchored per-sample timestamps
 *
 * Design overview:
 * ================
 * The ISR stamps every ring-buffer sample with a hardware tick counter
 * (incremented at 8000 Hz, 125 us per tick).  This module anchors that tick
 * counter to a real wall-clock time obtained from the Raspberry Pi's SNTP
 * server.  After anchoring, any tick value can be converted to a UTC
 * microsecond timestamp with sub-millisecond accuracy using simple arithmetic:
 *
 *   wall_us = anchor_wall_us + (tick - anchor_tick) * 125
 *
 * This means SNTP never touches the ISR at all.  The ISR remains unchanged;
 * only the data processing task calls this module.
 *
 * Clock drift correction:
 * =======================
 * The ESP32's crystal oscillates with ±50 ppm typical drift, accumulating
 * up to 1.5 ms of error over 30 seconds.  SNTP re-syncs on that interval and
 * sntp_sync_refresh_anchor() re-anchors the tick<->wall mapping after each
 * sync, so accumulated drift is bounded to one sync interval worth of error.
 *
 * SNTP sync mode is SMOOTH (slew-only, never step) so the anchor math is
 * never invalidated by a sudden clock jump.
 *
 * Usage:
 * ======
 *  1. Call sntp_sync_init() once, after ethernet is up and mdns is ready.
 *  2. Call sntp_sync_refresh_anchor() periodically from the data processing
 *     task (once per processing loop is fine -- it is very cheap).
 *  3. Call sntp_tick_to_wall_us(tick) to convert any ring-buffer tick to UTC.
 *  4. Call sntp_format_iso8601(tick, buf, len) to get an ISO-8601 string.
 */

#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

/**
 * How often the SNTP client polls the server (milliseconds).
 *
 * At 50 ppm crystal drift the ESP32 accumulates ~1.5 ms over 30 s, which is
 * well within the ±1 ms SNTP accuracy budget.  Increase if you want to reduce
 * network traffic; decrease toward 10 000 if you need tighter sync.
 */
#define SNTP_SYNC_POLL_INTERVAL_MS      30000

/**
 * SNTP server hostname.  "raspberrypi.local" is resolved by the mDNS stack
 * that is already initialised by mqtt_mdns_init() -- no static IP required.
 * If the Pi's hostname has been changed, update this to match.
 */
#define SNTP_SERVER_HOSTNAME            "raspberrypi.local"

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the SNTP client and start background synchronisation.
 *
 * Must be called AFTER:
 *   - ethernet_wait_for_ip() has returned ESP_OK
 *   - mqtt_mdns_init() has been called (so mDNS is ready to resolve hostnames)
 *
 * The first successful sync fires the internal callback which populates the
 * tick anchor.  sntp_sync_is_valid() returns false until that first sync
 * completes (typically 1--3 seconds after calling this function).
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sntp_sync_init(void);

/**
 * @brief Returns true once at least one successful SNTP sync has occurred.
 *
 * Timestamps produced before this returns true are based on a fallback
 * tick-relative format (no UTC wall time available yet).
 */
bool sntp_sync_is_valid(void);

/**
 * @brief Refresh the tick<->wall anchor point from the current system clock.
 *
 * Call this once per data-processing loop iteration.  It is cheap (two
 * syscalls) and keeps the anchor fresh between SNTP callbacks so that
 * ongoing crystal drift is corrected incrementally with each SNTP slew step
 * rather than only at the moment the SNTP callback fires.
 *
 * Safe to call before sntp_sync_is_valid() -- it is a no-op in that case.
 */
void sntp_sync_refresh_anchor(void);

/**
 * @brief Convert a ring-buffer tick value to microseconds since Unix epoch.
 *
 * @param tick  The tick field from an adxl355_raw_sample_t / scl3300_raw_sample_t.
 * @return      UTC time in microseconds since 1970-01-01T00:00:00Z,
 *              or 0 if no valid sync anchor exists yet.
 */
int64_t sntp_tick_to_wall_us(uint32_t tick);

/**
 * @brief Format a ring-buffer tick as an ISO-8601 UTC timestamp string.
 *
 * Output format (always UTC, microsecond precision):
 *   "2024-11-07T13:45:22.123456Z"
 *
 * If no sync anchor is available the output is a tick-relative string:
 *   "tick:12345678"  (indicates "not yet synchronised")
 *
 * @param tick  Ring-buffer tick value to convert.
 * @param buf   Output buffer.
 * @param len   Size of output buffer (at least 32 bytes recommended).
 * @return      Number of characters written (excluding null terminator),
 *              as from snprintf.
 */
int sntp_format_iso8601(uint32_t tick, char *buf, size_t len);

/**
 * @brief Return a human-readable sync status string for logging/diagnostics.
 *
 * Examples:
 *   "synced (30 s interval, last sync 8 s ago)"
 *   "waiting for first sync"
 */
const char *sntp_sync_status_str(void);

#ifdef __cplusplus
}
#endif

#endif /* SNTP_SYNC_H */
