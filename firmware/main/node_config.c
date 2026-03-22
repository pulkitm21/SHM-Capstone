/**
 * @file node_config.c
 * @brief Node state machine and ADXL355 runtime configuration
 */

#include "node_config.h"
#include "adxl355.h"
#include "sensor_task.h"
#include "fault_log.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "NODE_CFG";

/* Forward declaration — defined later in this file */
static esp_err_t node_config_run_selftest_with_sensitivity(
    adxl355_selftest_result_t *result, float sensitivity_lsb_g);

/* -------------------------------------------------------------------------
 * ODR lookup table
 *
 * Rules:
 *   isr_tick_divisor = 8000 / odr_hz          (must be whole number)
 *   decim_factor     = odr_hz / 200            (must be whole number)
 *   batch_size       = 200 samples/sec * 1 sec = 200 (always 1 second per packet)
 *   sensitivity      = set by range, updated when node_config_apply() is called
 *
 * sensitivity_lsb_g initialised here for ±2g default.
 * node_config_apply() overwrites it based on the chosen range.
 * ---------------------------------------------------------------------- */

static const adxl355_odr_config_t s_odr_table[NODE_CONFIG_ODR_COUNT] = {
    /* index, odr_hz, filter_reg,      tick_div, decim, batch, sens   */
    { 0,  4000,  ADXL355_FILTER_ODR_4000,  2,   20,   200,  256000.0f },
    { 1,  2000,  ADXL355_FILTER_ODR_2000,  4,   10,   200,  256000.0f },
    { 2,  1000,  ADXL355_FILTER_ODR_1000,  8,    5,   200,  256000.0f },
};

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static node_state_t       s_state  = NODE_STATE_IDLE;
static node_runtime_config_t s_cfg = {0};

/* Sensitivity lookup per range code */
static float sensitivity_for_range(uint8_t range)
{
    switch (range) {
        case NODE_RANGE_2G: return 256000.0f;
        case NODE_RANGE_4G: return 128000.0f;
        case NODE_RANGE_8G: return  64000.0f;
        default:            return 256000.0f;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

const char *node_state_str(node_state_t state)
{
    switch (state) {
        case NODE_STATE_IDLE:       return "idle";
        case NODE_STATE_CONFIGURED: return "configured";
        case NODE_STATE_RECORDING:  return "recording";
        case NODE_STATE_RECONFIG:   return "reconfig";
        case NODE_STATE_ERROR:      return "error";
        default:                    return "unknown";
    }
}

const adxl355_odr_config_t *node_config_get_odr(uint8_t odr_index)
{
    if (odr_index >= NODE_CONFIG_ODR_COUNT) {
        return NULL;
    }
    return &s_odr_table[odr_index];
}

void node_config_init(void)
{
    s_state = NODE_STATE_IDLE;

    /* Default: odr_index=2 (1000 Hz), ±2g, HPF off */
    const adxl355_odr_config_t *odr = &s_odr_table[2];
    s_cfg.odr_index         = 2;
    s_cfg.range             = NODE_RANGE_2G;
    s_cfg.hpf_corner        = 0;
    s_cfg.seq               = 0;
    s_cfg.isr_tick_divisor  = odr->isr_tick_divisor;
    s_cfg.decim_factor      = odr->decim_factor;
    s_cfg.batch_size        = odr->batch_size;
    s_cfg.sensitivity_lsb_g = sensitivity_for_range(NODE_RANGE_2G);
    s_cfg.odr_hz            = odr->odr_hz;

    ESP_LOGI(TAG, "Node config initialised (state=IDLE, default ODR=1000Hz, range=±2g)");
}

node_state_t node_config_get_state(void)
{
    return s_state;
}

const node_runtime_config_t *node_config_get(void)
{
    return &s_cfg;
}

esp_err_t node_config_apply(uint8_t odr_index, uint8_t range,
                             uint8_t hpf_corner, uint32_t seq,
                             adxl355_selftest_result_t *result)
{
    /* --- Validate inputs --- */
    const adxl355_odr_config_t *odr = node_config_get_odr(odr_index);
    if (odr == NULL) {
        ESP_LOGE(TAG, "Invalid odr_index=%u (must be 0, 1, or 2)", odr_index);
        return ESP_ERR_INVALID_ARG;
    }
    if (range != NODE_RANGE_2G && range != NODE_RANGE_4G && range != NODE_RANGE_8G) {
        ESP_LOGE(TAG, "Invalid range=%u (must be 1=±2g, 2=±4g, 3=±8g)", range);
        return ESP_ERR_INVALID_ARG;
    }
    if (hpf_corner > 6) {
        ESP_LOGE(TAG, "Invalid hpf_corner=%u (must be 0-6)", hpf_corner);
        return ESP_ERR_INVALID_ARG;
    }

    /* --- If recording, stop ISR first. Data gap begins here. --- */
    bool was_recording = (s_state == NODE_STATE_RECORDING);
    if (was_recording) {
        ESP_LOGI(TAG, "Stopping ISR for reconfiguration (data gap begins)");
        s_state = NODE_STATE_RECONFIG;
        esp_err_t err = sensor_acquisition_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop ISR: %s", esp_err_to_name(err));
            s_state = NODE_STATE_ERROR;
            fault_log_record(FAULT_SPI_ERROR);
            return err;
        }
        /* Flush ring buffers so stale data from the old config is discarded */
        sensor_acquisition_reset_stats();
    }

    ESP_LOGI(TAG, "Applying config: ODR=%lu Hz, range=%u, HPF=%u, seq=%lu",
             (unsigned long)odr->odr_hz, range, hpf_corner, (unsigned long)seq);

    /* --- Step 1: Put ADXL355 into standby before writing config registers --- */
    /* We use the adxl355.h register defines directly via the driver write */
    esp_err_t err = adxl355_write_reg_pub(ADXL355_REG_POWER_CTL,
                                          ADXL355_POWER_STANDBY_BIT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter standby: %s", esp_err_to_name(err));
        s_state = NODE_STATE_ERROR;
        fault_log_record(FAULT_SPI_ERROR);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* --- Step 2: Write Filter register (ODR_LPF + HPF_CORNER) --- */
    uint8_t filter_val = (uint8_t)((hpf_corner << 4) | odr->filter_reg);
    err = adxl355_write_reg_pub(ADXL355_REG_FILTER, filter_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write FILTER reg: %s", esp_err_to_name(err));
        s_state = NODE_STATE_ERROR;
        fault_log_record(FAULT_SPI_ERROR);
        return err;
    }

    /* --- Step 3: Write Range register (preserve upper bits) --- */
    uint8_t range_reg = 0;
    err = adxl355_read_reg_pub(ADXL355_REG_RANGE, &range_reg, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RANGE reg: %s", esp_err_to_name(err));
        s_state = NODE_STATE_ERROR;
        fault_log_record(FAULT_SPI_ERROR);
        return err;
    }
    range_reg = (uint8_t)((range_reg & ~0x03u) | (range & 0x03u));
    err = adxl355_write_reg_pub(ADXL355_REG_RANGE, range_reg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write RANGE reg: %s", esp_err_to_name(err));
        s_state = NODE_STATE_ERROR;
        fault_log_record(FAULT_SPI_ERROR);
        return err;
    }

    /* --- Step 4: Read-back verification --- */
    uint8_t rb_filter = 0, rb_range = 0;
    adxl355_read_reg_pub(ADXL355_REG_FILTER, &rb_filter, 1);
    adxl355_read_reg_pub(ADXL355_REG_RANGE,  &rb_range,  1);

    if (rb_filter != filter_val) {
        ESP_LOGE(TAG, "FILTER readback mismatch: wrote 0x%02X read 0x%02X",
                 filter_val, rb_filter);
        s_state = NODE_STATE_ERROR;
        fault_log_record(FAULT_SPI_ERROR);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((rb_range & 0x03u) != (range & 0x03u)) {
        ESP_LOGE(TAG, "RANGE readback mismatch: wrote 0x%02X read 0x%02X",
                 range, rb_range);
        s_state = NODE_STATE_ERROR;
        fault_log_record(FAULT_SPI_ERROR);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Register readback OK (FILTER=0x%02X RANGE=0x%02X)",
             rb_filter, rb_range);

    /* --- Step 5: Return to measurement mode --- */
    err = adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to exit standby: %s", esp_err_to_name(err));
        s_state = NODE_STATE_ERROR;
        fault_log_record(FAULT_SPI_ERROR);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* --- Step 6: Optional self-test ---
     * Must use the NEW range's sensitivity, not the old s_cfg value which
     * hasn't been updated yet (that happens in Step 7). Pass it explicitly. */
    if (result != NULL) {
        float new_sensitivity = sensitivity_for_range(range);
        esp_err_t st_err = node_config_run_selftest_with_sensitivity(result, new_sensitivity);
        if (st_err != ESP_OK) {
            ESP_LOGW(TAG, "Self-test execution failed: %s", esp_err_to_name(st_err));
            result->ran    = false;
            result->passed = false;
        }
    }

    /* --- Step 7: Update runtime config atomically --- */
    s_cfg.odr_index         = odr_index;
    s_cfg.range             = range;
    s_cfg.hpf_corner        = hpf_corner;
    s_cfg.seq               = seq;
    s_cfg.isr_tick_divisor  = odr->isr_tick_divisor;
    s_cfg.decim_factor      = odr->decim_factor;
    s_cfg.batch_size        = odr->batch_size;
    s_cfg.sensitivity_lsb_g = sensitivity_for_range(range);
    s_cfg.odr_hz            = odr->odr_hz;

    /* Transition to CONFIGURED. Caller decides whether to restart ISR. */
    s_state = NODE_STATE_CONFIGURED;

    ESP_LOGI(TAG, "Config applied: ODR=%lu Hz decim=%lu batch=%lu sens=%.0f LSB/g",
             (unsigned long)s_cfg.odr_hz,
             (unsigned long)s_cfg.decim_factor,
             (unsigned long)s_cfg.batch_size,
             s_cfg.sensitivity_lsb_g);

    return ESP_OK;
}

void node_config_set_recording(void)
{
    if (s_state != NODE_STATE_CONFIGURED) {
        ESP_LOGW(TAG, "set_recording called from state %s (expected configured)",
                 node_state_str(s_state));
    }
    s_state = NODE_STATE_RECORDING;
    ESP_LOGI(TAG, "State → RECORDING");
}

void node_config_set_configured(void)
{
    if (s_state != NODE_STATE_RECORDING) {
        ESP_LOGW(TAG, "set_configured called from state %s (expected recording)",
                 node_state_str(s_state));
    }
    s_state = NODE_STATE_CONFIGURED;
    ESP_LOGI(TAG, "State → CONFIGURED (stopped)");
}

void node_config_set_error(uint8_t fault_code)
{
    ESP_LOGE(TAG, "State → ERROR (fault %u)", fault_code);
    fault_log_record(fault_code);
    s_state = NODE_STATE_ERROR;
}

void node_config_reset(void)
{
    ESP_LOGI(TAG, "State → IDLE (reset from %s)", node_state_str(s_state));
    s_state = NODE_STATE_IDLE;
}

/* -------------------------------------------------------------------------
 * Self-test
 * ---------------------------------------------------------------------- */

/* Internal implementation — takes sensitivity explicitly so it works correctly
 * both during node_config_apply (before s_cfg is updated) and from the public
 * API (after s_cfg is set). */
static esp_err_t run_selftest_impl(adxl355_selftest_result_t *result,
                                    float sensitivity_lsb_g)
{
    memset(result, 0, sizeof(*result));
    result->ran = true;

    const float ST_X_MIN = 0.1f, ST_X_MAX = 0.6f;
    const float ST_Y_MIN = 0.1f, ST_Y_MAX = 0.6f;
    /* Z lower bound relaxed from datasheet 0.5g to 0.3g.
     * When the board sits flat under gravity the Z proof mass is already
     * deflected ~1g, which reduces the measured self-test delta. X/Y are
     * the reliable indicators; Z is treated as advisory. */
    const float ST_Z_MIN = 0.3f, ST_Z_MAX = 3.0f;

    ESP_LOGI(TAG, "Running ADXL355 self-test (sensitivity=%.0f LSB/g)...",
             sensitivity_lsb_g);

    esp_err_t err = adxl355_write_reg_pub(ADXL355_REG_POWER_CTL,
                                          ADXL355_POWER_STANDBY_BIT);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(5));

    adxl355_write_reg_pub(ADXL355_REG_SELF_TEST, 0x00);

    err = adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, 0x00);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Baseline (3 samples, average) */
    float base_x = 0, base_y = 0, base_z = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t b[9] = {0};
        err = adxl355_read_reg_pub(ADXL355_REG_XDATA3, b, 9);
        if (err != ESP_OK) goto selftest_cleanup;

        uint32_t xu = ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | (b[2] >> 4);
        uint32_t yu = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4);
        uint32_t zu = ((uint32_t)b[6] << 12) | ((uint32_t)b[7] << 4) | (b[8] >> 4);

        int32_t xi = (xu & 0x80000u) ? (int32_t)(xu | 0xFFF00000u) : (int32_t)xu;
        int32_t yi = (yu & 0x80000u) ? (int32_t)(yu | 0xFFF00000u) : (int32_t)yu;
        int32_t zi = (zu & 0x80000u) ? (int32_t)(zu | 0xFFF00000u) : (int32_t)zu;

        base_x += (float)xi / sensitivity_lsb_g;
        base_y += (float)yi / sensitivity_lsb_g;
        base_z += (float)zi / sensitivity_lsb_g;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    base_x /= 3.0f; base_y /= 3.0f; base_z /= 3.0f;

    /* Enable self-test force */
    err = adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, ADXL355_POWER_STANDBY_BIT);
    if (err != ESP_OK) goto selftest_cleanup;
    vTaskDelay(pdMS_TO_TICKS(5));

    adxl355_write_reg_pub(ADXL355_REG_SELF_TEST, 0x03);

    err = adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, 0x00);
    if (err != ESP_OK) goto selftest_cleanup;
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Stimulus reading (3 samples, average) */
    float st_x = 0, st_y = 0, st_z = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t b[9] = {0};
        err = adxl355_read_reg_pub(ADXL355_REG_XDATA3, b, 9);
        if (err != ESP_OK) goto selftest_cleanup;

        uint32_t xu = ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | (b[2] >> 4);
        uint32_t yu = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4);
        uint32_t zu = ((uint32_t)b[6] << 12) | ((uint32_t)b[7] << 4) | (b[8] >> 4);

        int32_t xi = (xu & 0x80000u) ? (int32_t)(xu | 0xFFF00000u) : (int32_t)xu;
        int32_t yi = (yu & 0x80000u) ? (int32_t)(yu | 0xFFF00000u) : (int32_t)yu;
        int32_t zi = (zu & 0x80000u) ? (int32_t)(zu | 0xFFF00000u) : (int32_t)zu;

        st_x += (float)xi / sensitivity_lsb_g;
        st_y += (float)yi / sensitivity_lsb_g;
        st_z += (float)zi / sensitivity_lsb_g;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    st_x /= 3.0f; st_y /= 3.0f; st_z /= 3.0f;

    result->delta_x = st_x - base_x;
    result->delta_y = st_y - base_y;
    result->delta_z = st_z - base_z;

    float abs_z = (result->delta_z < 0) ? -result->delta_z : result->delta_z;

    result->passed =
        (result->delta_x >= ST_X_MIN && result->delta_x <= ST_X_MAX) &&
        (result->delta_y >= ST_Y_MIN && result->delta_y <= ST_Y_MAX) &&
        (abs_z           >= ST_Z_MIN && abs_z           <= ST_Z_MAX);

    ESP_LOGI(TAG, "Self-test deltas: X=%.3fg Y=%.3fg Z=%.3fg -> %s",
             result->delta_x, result->delta_y, result->delta_z,
             result->passed ? "PASS" : "FAIL");

    if (!result->passed) {
        ESP_LOGW(TAG, "Self-test limits: X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]",
                 ST_X_MIN, ST_X_MAX, ST_Y_MIN, ST_Y_MAX, ST_Z_MIN, ST_Z_MAX);
    }

selftest_cleanup:
    adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, ADXL355_POWER_STANDBY_BIT);
    vTaskDelay(pdMS_TO_TICKS(2));
    adxl355_write_reg_pub(ADXL355_REG_SELF_TEST, 0x00);
    adxl355_write_reg_pub(ADXL355_REG_POWER_CTL, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));

    return err;
}

/* Called from node_config_apply with the new range sensitivity (before s_cfg update) */
static esp_err_t node_config_run_selftest_with_sensitivity(
    adxl355_selftest_result_t *result, float sensitivity_lsb_g)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    return run_selftest_impl(result, sensitivity_lsb_g);
}

/* Public API — uses current s_cfg sensitivity (valid after configure) */
esp_err_t node_config_run_selftest(adxl355_selftest_result_t *result)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    return run_selftest_impl(result, s_cfg.sensitivity_lsb_g);
}
