/**
 * @file node_config.h
 * @brief Node state machine and runtime ADXL355 configuration
 *
 * Architecture
 * ============
 * The node boots into IDLE and waits for commands from the Raspberry Pi
 * over MQTT. State transitions:
 *
 *   IDLE ──(configure)──► CONFIGURED ──(start)──► RECORDING
 *                              ▲                       │
 *                              └──(reconfigure)─────────┤ (stop)
 *                                                       ▼
 *                                                  CONFIGURED
 *
 *   Any state ──(sensor fault / init fail)──► ERROR
 *   ERROR ──(reset cmd)──► IDLE
 *
 * Reconfiguration always stops the ISR first. In-flight data is discarded.
 * A data gap is expected and documented behaviour.
 *
 * Supported ODR settings (output always 200 Hz, decimation = ODR / 200)
 * ======================================================================
 *   odr_index 0 → 4000 Hz ODR, decimation 20
 *   odr_index 1 → 2000 Hz ODR, decimation 10
 *   odr_index 2 → 1000 Hz ODR, decimation  5  ← default
 *
 * ODR values 500/250/125 Hz are NOT supported because ODR/200 is not a
 * whole number, which would make the decimation logic incorrect.
 *
 * Self-test
 * =========
 * Run during configure/reconfigure. Puts ADXL355 into standby, enables
 * ST1+ST2 (register 0x2E), reads X/Y/Z delta, validates against datasheet
 * limits, then clears ST bits. Result reported in the status ACK packet.
 */

#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Node state machine
 * ---------------------------------------------------------------------- */

typedef enum {
    NODE_STATE_IDLE        = 0,   /**< Boot default. Sensors init'd, ISR off. */
    NODE_STATE_CONFIGURED  = 1,   /**< Settings applied. Waiting for start.   */
    NODE_STATE_RECORDING   = 2,   /**< ISR running, MQTT publishing.           */
    NODE_STATE_RECONFIG    = 3,   /**< Transient: ISR stopped, applying config.*/
    NODE_STATE_ERROR       = 4,   /**< Sensor fault. Needs reset cmd.          */
} node_state_t;

/** Human-readable state name (for logging / status JSON). */
const char *node_state_str(node_state_t state);

/* -------------------------------------------------------------------------
 * ADXL355 configuration table
 *
 * All values derived from the fixed 200 Hz output requirement.
 * Do NOT add entries where odr_hz % 200 != 0.
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t  odr_index;          /**< 0/1/2 — index sent by Pi frontend      */
    uint32_t odr_hz;             /**< ADXL355 output data rate in Hz          */
    uint8_t  filter_reg;         /**< Value for register 0x28 (ODR_LPF field) */
    uint32_t isr_tick_divisor;   /**< BASE_TIMER_FREQ_HZ (8000) / odr_hz      */
    uint32_t decim_factor;       /**< Raw samples averaged per output sample   */
    uint32_t batch_size;         /**< Output samples per MQTT packet (1 sec)   */
    float    sensitivity_lsb_g;  /**< LSB/g for the configured range           */
} adxl355_odr_config_t;

/** Number of valid ODR table entries. */
#define NODE_CONFIG_ODR_COUNT  3

/** Lookup an ODR config by odr_index. Returns NULL for invalid index. */
const adxl355_odr_config_t *node_config_get_odr(uint8_t odr_index);

/* -------------------------------------------------------------------------
 * Runtime configuration (applied atomically during reconfiguration)
 * ---------------------------------------------------------------------- */

/** Valid range codes — match ADXL355_RANGE_* in adxl355.h */
#define NODE_RANGE_2G   1
#define NODE_RANGE_4G   2
#define NODE_RANGE_8G   3

typedef struct {
    uint8_t  odr_index;          /**< Index into ODR table (0/1/2)            */
    uint8_t  range;              /**< 1=±2g, 2=±4g, 3=±8g                    */
    uint8_t  hpf_corner;        /**< HPF corner: 0=off, 1-6 per datasheet     */
    uint32_t seq;                /**< Echoed back in ACK                       */

    /* Derived from ODR table — updated atomically with the above */
    uint32_t isr_tick_divisor;
    uint32_t decim_factor;
    uint32_t batch_size;
    float    sensitivity_lsb_g;
    uint32_t odr_hz;
} node_runtime_config_t;

/* -------------------------------------------------------------------------
 * Self-test result
 * ---------------------------------------------------------------------- */

typedef struct {
    bool    ran;           /**< Was self-test executed?            */
    bool    passed;        /**< Did all axes pass?                 */
    float   delta_x;       /**< Measured delta on X axis (g)       */
    float   delta_y;       /**< Measured delta on Y axis (g)       */
    float   delta_z;       /**< Measured delta on Z axis (g)       */
} adxl355_selftest_result_t;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the node config module.
 *
 * Sets state to IDLE, loads default config (odr_index=2, range=±2g, hpf=off).
 * Must be called once before any other node_config_* function.
 */
void node_config_init(void);

/** @brief Return the current node state. */
node_state_t node_config_get_state(void);

/** @brief Return a pointer to the current runtime config (read-only). */
const node_runtime_config_t *node_config_get(void);

/**
 * @brief Apply a new configuration and transition state.
 *
 * Validates odr_index, range, hpf_corner. If valid:
 *   - If currently RECORDING: stops ISR, flushes buffers, enters RECONFIG.
 *   - Puts ADXL355 in standby, writes filter + range registers, reads back.
 *   - Optionally runs self-test.
 *   - Restores measurement mode and updates runtime config atomically.
 *   - Transitions to CONFIGURED.
 *
 * The caller is responsible for restarting the ISR (call
 * sensor_acquisition_start()) and transitioning to RECORDING if the node
 * was previously recording.
 *
 * @param odr_index  0=4kHz, 1=2kHz, 2=1kHz
 * @param range      NODE_RANGE_2G / _4G / _8G
 * @param hpf_corner 0=off, 1-6 per datasheet Table 44
 * @param seq        Sequence number echoed in ACK
 * @param[out] result Self-test result (may be NULL to skip self-test)
 * @return ESP_OK on success, error code on failure → state becomes ERROR.
 */
esp_err_t node_config_apply(uint8_t odr_index, uint8_t range,
                             uint8_t hpf_corner, uint32_t seq,
                             adxl355_selftest_result_t *result);

/**
 * @brief Transition to RECORDING state.
 * Must only be called from CONFIGURED state.
 * The caller must have already called sensor_acquisition_start().
 */
void node_config_set_recording(void);

/**
 * @brief Transition to CONFIGURED state (e.g. after stop command).
 * Must only be called from RECORDING state.
 * The caller must have already called sensor_acquisition_stop().
 */
void node_config_set_configured(void);

/**
 * @brief Transition to ERROR state with a reason code.
 * @param fault_code One of the FAULT_* defines from fault_log.h
 */
void node_config_set_error(uint8_t fault_code);

/**
 * @brief Attempt recovery: transition ERROR → IDLE.
 * Does not re-init sensors. Caller should re-init if needed.
 */
void node_config_reset(void);

/**
 * @brief Run ADXL355 self-test sequence.
 *
 * Sensor must be in measurement mode before calling.
 * Briefly enters standby, enables ST1+ST2, reads deltas, clears ST bits,
 * returns to measurement mode.
 *
 * Datasheet expected output change (Table 2):
 *   X-axis: 0.1 – 0.6 g
 *   Y-axis: 0.1 – 0.6 g
 *   Z-axis: 0.5 – 3.0 g
 *
 * @param[out] result Filled with measured deltas and pass/fail.
 * @return ESP_OK on success (result->passed may still be false).
 */
esp_err_t node_config_run_selftest(adxl355_selftest_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* NODE_CONFIG_H */
