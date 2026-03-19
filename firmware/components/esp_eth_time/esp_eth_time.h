/*
 * esp_eth_time.h — Software PTP stub for ESP32 (no hardware IEEE 1588)
 *
 * The original esp_eth_time component requires SOC_EMAC_IEEE1588V2_SUPPORTED
 * which is not present on the original ESP32. This stub replaces all hardware
 * clock operations with CLOCK_REALTIME so the rest of the codebase compiles
 * and runs unchanged. Accuracy is 1-5ms (software PTP) instead of 50-200us.
 */

#pragma once

#include <time.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Use clock ID 1 (CLOCK_REALTIME) — no hardware PTP clock on ESP32 */
#define CLOCK_PTP_SYSTEM    CLOCK_REALTIME

typedef struct {
    esp_eth_handle_t eth_hndl;
} esp_eth_clock_cfg_t;

typedef enum {
    ETH_CLK_ADJ_FREQ_SCALE,
} esp_eth_clock_adj_mode_t;

typedef struct {
    esp_eth_clock_adj_mode_t mode;
    double freq_scale;
} esp_eth_clock_adj_param_t;

/*
 * All functions below are stubs that forward to CLOCK_REALTIME.
 * esp_eth_clock_adjtime is a no-op stub — clock discipline is handled
 * inside ptpd.c using POSIX adjtime() directly.
 */

static inline int esp_eth_clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;
    return clock_gettime(CLOCK_REALTIME, tp);
}

static inline int esp_eth_clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    (void)clock_id;
    return clock_settime(CLOCK_REALTIME, tp);
}

static inline int esp_eth_clock_adjtime(clockid_t clock_id, esp_eth_clock_adj_param_t *adj)
{
    /* No-op stub. Software PTP clock discipline is done via adjtime()
     * directly inside ptpd.c ptp_adjtime(). */
    (void)clock_id;
    (void)adj;
    return 0;
}

static inline esp_err_t esp_eth_clock_init(clockid_t clock_id, esp_eth_clock_cfg_t *cfg)
{
    /* No hardware clock to initialize on ESP32 */
    (void)clock_id;
    (void)cfg;
    return ESP_OK;
}

static inline int esp_eth_clock_set_target_time(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;
    (void)tp;
    return 0;
}

#ifdef __cplusplus
}
#endif
