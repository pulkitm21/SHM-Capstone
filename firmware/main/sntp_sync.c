/**
 * @file sntp_sync.c
 * @brief SNTP clock synchronisation with tick-anchored per-sample timestamps
 *
 * Implementation notes:
 * =====================
 * Anchor data is stored in two volatile 64-bit integers.  They are written
 * only from two places:
 *
 *   1. The SNTP notification callback (lwIP thread context).
 *   2. sntp_sync_refresh_anchor() (data-processing task context).
 *
 * Both callers are non-ISR and execute on the same CPU core (APP_CPU), so
 * a simple critical section (portENTER_CRITICAL) is sufficient for
 * atomicity.  The ISR only reads get_tick_count() and never touches the
 * anchor variables.
 *
 * Tick arithmetic uses int32_t for the delta so that uint32_t wrap-around
 * (after ~149 hours at 8000 Hz) is handled correctly by two's-complement
 * subtraction -- as long as the delta between a sample tick and the anchor
 * tick is less than 2^31 ticks (~74.5 hours), the result is exact.
 */

#include "sntp_sync.h"
#include "sensor_task.h"        /* get_tick_count() */

#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

static const char *TAG = "SNTP";

/* -------------------------------------------------------------------------
 * Anchor state
 * ---------------------------------------------------------------------- */

/* Each tick is 125 µs (8000 Hz timer). */
#define US_PER_TICK     125

/*
 * Anchor: a simultaneous snapshot of (wall clock µs, tick counter) taken
 * at the moment of each SNTP sync notification or anchor refresh.
 *
 * Written under a spinlock; read lock-free by sntp_tick_to_wall_us() since
 * reads happen on the same core and the compiler barrier is enough.
 */
static portMUX_TYPE        s_anchor_mux    = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t    s_anchor_wall_us = 0;   /* UTC µs at anchor */
static volatile uint32_t   s_anchor_tick    = 0;   /* tick counter at anchor */
static volatile bool       s_anchor_valid   = false;

/* Diagnostics */
static volatile uint32_t   s_sync_count     = 0;   /* total SNTP syncs */
static volatile int64_t    s_last_sync_us   = 0;   /* esp_timer_get_time() at last sync */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/**
 * Update the anchor.  Called from the SNTP callback AND from
 * sntp_sync_refresh_anchor().  Both paths supply a freshly read
 * gettimeofday() result and the corresponding tick value.
 */
static void update_anchor(int64_t wall_us, uint32_t tick)
{
    portENTER_CRITICAL(&s_anchor_mux);
    s_anchor_wall_us = wall_us;
    s_anchor_tick    = tick;
    s_anchor_valid   = true;
    portEXIT_CRITICAL(&s_anchor_mux);
}

/* -------------------------------------------------------------------------
 * SNTP notification callback
 *
 * Fired by the lwIP SNTP client immediately after it adjusts the system
 * clock.  The struct timeval argument reflects the new (post-adjustment)
 * time, so we use it directly rather than calling gettimeofday() again,
 * which avoids a tiny race window.
 * ---------------------------------------------------------------------- */
static void sntp_sync_notification_cb(struct timeval *tv)
{
    /* Snapshot the tick counter as close as possible to the clock update. */
    uint32_t tick = get_tick_count();

    int64_t wall_us = (int64_t)tv->tv_sec * 1000000LL + (int64_t)tv->tv_usec;

    update_anchor(wall_us, tick);

    s_sync_count++;
    s_last_sync_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Sync #%lu: wall=%" PRId64 " us, tick=%lu, offset=~%.3f ms",
             (unsigned long)s_sync_count,
             wall_us,
             (unsigned long)tick,
             /* Rough offset: difference between the new wall time and what the
              * old anchor would have predicted.  Zero on first sync. */
             (s_sync_count > 1)
                 ? (double)(wall_us -
                     (s_anchor_wall_us +
                      (int64_t)(int32_t)(tick - s_anchor_tick) * US_PER_TICK)) / 1000.0
                 : 0.0);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t sntp_sync_init(void)
{
    ESP_LOGI(TAG, "Initialising SNTP client");
    ESP_LOGI(TAG, "  Server:        %s", SNTP_SERVER_HOSTNAME);
    ESP_LOGI(TAG, "  Poll interval: %d ms", SNTP_SYNC_POLL_INTERVAL_MS);
    ESP_LOGI(TAG, "  Sync mode:     SMOOTH (slew, no steps)");

    /*
     * SMOOTH mode distributes clock corrections gradually so the anchor
     * arithmetic is never invalidated by a sudden time jump.
     * IMMED (step) mode would make previously computed tick-to-wall offsets
     * wrong by the size of the correction step.
     */
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    /*
     * The Pi is already reachable as "raspberrypi.local" via the mDNS stack
     * initialised by mqtt_mdns_init() -- no static IP, no extra config.
     */
    esp_sntp_setservername(0, SNTP_SERVER_HOSTNAME);

    /* Set the poll interval before calling esp_sntp_init(). */
    sntp_set_sync_interval(SNTP_SYNC_POLL_INTERVAL_MS);

    sntp_set_time_sync_notification_cb(sntp_sync_notification_cb);

    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP client started -- waiting for first sync from %s",
             SNTP_SERVER_HOSTNAME);

    return ESP_OK;
}

bool sntp_sync_is_valid(void)
{
    return s_anchor_valid;
}

void sntp_sync_refresh_anchor(void)
{
    /*
     * Re-anchor every processing cycle.  Between SNTP callbacks the system
     * clock is being slewed by the lwIP SNTP client in tiny increments.
     * Refreshing here means our tick->wall conversion always reflects the
     * latest slew adjustment rather than being frozen at the last callback.
     */
    if (!s_anchor_valid) {
        return; /* nothing to refresh -- first sync hasn't happened yet */
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t tick = get_tick_count();

    int64_t wall_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
    update_anchor(wall_us, tick);
}

int64_t sntp_tick_to_wall_us(uint32_t tick)
{
    if (!s_anchor_valid) {
        return 0;
    }

    /*
     * Read anchor atomically.  We use a local copy so the critical section
     * is as short as possible -- just two 32/64-bit reads.
     */
    int64_t  anchor_wall;
    uint32_t anchor_tick;

    portENTER_CRITICAL(&s_anchor_mux);
    anchor_wall = s_anchor_wall_us;
    anchor_tick = s_anchor_tick;
    portEXIT_CRITICAL(&s_anchor_mux);

    /*
     * Cast delta to int32_t so two's-complement subtraction gives the correct
     * signed result across uint32_t wrap-around boundaries.
     * Valid as long as |tick - anchor_tick| < 2^31 ticks (~74.5 hours).
     */
    int64_t delta_us = (int64_t)(int32_t)(tick - anchor_tick) * (int64_t)US_PER_TICK;

    return anchor_wall + delta_us;
}

int sntp_format_iso8601(uint32_t tick, char *buf, size_t len)
{
    if (!s_anchor_valid) {
        /* No sync yet: emit a tick-relative placeholder so the Raspberry Pi
         * can still ingest packets with a clearly-marked unsynchronised field. */
        return snprintf(buf, len, "tick:%" PRIu32, tick);
    }

    int64_t wall_us = sntp_tick_to_wall_us(tick);
    if (wall_us <= 0) {
        return snprintf(buf, len, "tick:%" PRIu32, tick);
    }

    time_t  sec = (time_t)(wall_us / 1000000LL);
    int     us  = (int)(wall_us % 1000000LL);

    /* Protect against negative modulo on platforms where % can be negative. */
    if (us < 0) {
        us  += 1000000;
        sec -= 1;
    }

    struct tm t;
    gmtime_r(&sec, &t);

    /* Write "YYYY-MM-DDTHH:MM:SS" (19 chars) into buf first. */
    size_t base_len = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &t);
    if (base_len == 0) {
        return snprintf(buf, len, "tick:%" PRIu32, tick);
    }

    /* Append ".XXXXXXZ" microseconds + Z suffix. */
    return (int)base_len + snprintf(buf + base_len, len - base_len,
                                    ".%06dZ", us);
}

const char *sntp_sync_status_str(void)
{
    static char s_status[80];

    if (!s_anchor_valid) {
        snprintf(s_status, sizeof(s_status), "waiting for first sync from %s",
                 SNTP_SERVER_HOSTNAME);
    } else {
        int64_t age_ms = (esp_timer_get_time() - s_last_sync_us) / 1000;
        snprintf(s_status, sizeof(s_status),
                 "synced (%d s interval, last sync %" PRId64 " ms ago, total syncs: %lu)",
                 SNTP_SYNC_POLL_INTERVAL_MS / 1000,
                 age_ms,
                 (unsigned long)s_sync_count);
    }

    return s_status;
}
