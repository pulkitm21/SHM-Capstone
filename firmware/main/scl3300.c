/**
 * @file scl3300.c
 * @brief SCL3300-D01 Inclinometer Driver (SPI) - manual CS version
 *
 * This version uses MANUAL chip select for SCL3300 because automatic CS handling
 * on the shared SPI bus was interfering with ADXL355 transactions.
 *
 * Features:
 *  - Correct SPI command frames from Table 15
 *  - Proper off-frame protocol handling
 *  - Complete startup sequence from Table 11
 *  - Angle output
 *  - Error checking and logging
 *
 * Performance constraints:
 *  - No heap allocation in the read path
 *  - No logging in the high-rate read path
 */

#include "scl3300.h"
#include "spi_bus.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <string.h>
#include <inttypes.h>

static const char *TAG = "SCL3300";

// SPI device handle
spi_device_handle_t scl3300_spi_handle = NULL;   // Exposed for ISR
static spi_device_handle_t s_scl3300 = NULL;     // Internal handle for non-ISR use

// Acceleration scale factors for each mode (LSB/g)
static const float ACCEL_SCALE_MODE1 = 6000.0f;

// Current mode (default Mode 1)
static float s_current_accel_scale = ACCEL_SCALE_MODE1;

/* ---------- Manual CS helpers ---------- */

static inline void scl3300_cs_init(void)
{
    gpio_set_direction(SPI_CS_SCL3300_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(SPI_CS_SCL3300_IO, 1);   // deselected
}

static inline void scl3300_cs_low(void)
{
    gpio_set_level(SPI_CS_SCL3300_IO, 0);
}

static inline void scl3300_cs_high(void)
{
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
}

/**
 * @brief Transfer a 32-bit command and receive a 32-bit response
 *
 * Uses SPI_TRANS_USE_TXDATA/RXDATA to avoid dynamic allocation.
 * Uses MANUAL CS.
 *
 * @param cmd 32-bit command with CRC
 * @param response Pointer to store 32-bit response (can be NULL)
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t scl3300_transfer(uint32_t cmd, uint32_t *response)
{
    if (s_scl3300 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 32;
    t.rxlength = 32;

    // Pack command
    t.tx_data[0] = (uint8_t)((cmd >> 24) & 0xFF);
    t.tx_data[1] = (uint8_t)((cmd >> 16) & 0xFF);
    t.tx_data[2] = (uint8_t)((cmd >> 8) & 0xFF);
    t.tx_data[3] = (uint8_t)(cmd & 0xFF);

    scl3300_cs_low();
    esp_err_t ret = spi_device_polling_transmit(s_scl3300, &t);
    scl3300_cs_high();

    if (ret != ESP_OK) {
        return ret;
    }

    if (response) {
        *response = ((uint32_t)t.rx_data[0] << 24) |
                    ((uint32_t)t.rx_data[1] << 16) |
                    ((uint32_t)t.rx_data[2] << 8)  |
                    (uint32_t)t.rx_data[3];
    }

    return ESP_OK;
}

/**
 * @brief Read 16-bit data using the off-frame protocol
 *
 * The device returns the response to the previous command.
 * Pattern: send cmd, then send cmd again.
 *
 * @param cmd 32-bit read command
 * @param out Pointer to store 16-bit result
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t scl3300_read_data16(uint32_t cmd, int16_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t resp = 0;

    // Prime: send command
    esp_err_t ret = scl3300_transfer(cmd, &resp);
    if (ret != ESP_OK) return ret;

    // Fetch: send command again to get response
    ret = scl3300_transfer(cmd, &resp);
    if (ret != ESP_OK) return ret;

    // Extract middle 16 bits [23:8]
    *out = (int16_t)((resp >> 8) & 0xFFFF);
    return ESP_OK;
}

/**
 * @brief Extract Return Status (RS) bits from SPI response
 *
 * RS bits are at [25:24] in the 32-bit SPI response
 *
 * @param spi_response 32-bit SPI response
 * @return RS value (0x00, 0x01, 0x02, or 0x03)
 */
static uint8_t scl3300_extract_rs(uint32_t spi_response)
{
    return (uint8_t)((spi_response >> 24) & 0x03u);
}

/**
 * @brief Initialize the SCL3300 inclinometer
 *
 * Follows datasheet Table 11 startup sequence precisely
 */
esp_err_t scl3300_init(void)
{
    ESP_LOGI(TAG, "Initializing SCL3300-D01 inclinometer...");

    // Add device to SPI bus if not already added
    if (s_scl3300 == NULL) {
        scl3300_cs_init();

        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = SCL3300_SPI_CLOCK_HZ,
            .mode = 0,              // CPOL=0, CPHA=0
            .spics_io_num = -1,     // manual CS
            .queue_size = 1,
            .command_bits = 0,
            .address_bits = 0,
        };

        esp_err_t ret = spi_bus_add_device(spi_bus_get_host(), &devcfg, &s_scl3300);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
            return ret;
        }

        scl3300_spi_handle = s_scl3300;  // Expose handle for ISR
    }

    // === Datasheet Table 11: Start-up Sequence ===

    // Step 1: Power-on delay (25ms)
    ESP_LOGI(TAG, "Step 1: Power-on delay (25ms)...");
    vTaskDelay(pdMS_TO_TICKS(25));

    uint32_t resp = 0;
    esp_err_t ret;

    // Step 2: Software Reset
    ESP_LOGI(TAG, "Step 2: Software Reset...");
    ret = scl3300_transfer(SCL3300_CMD_SW_RESET, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SW_RESET failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 3: Wait 1ms after reset
    ESP_LOGI(TAG, "Step 3: Wait 1ms after reset...");
    vTaskDelay(pdMS_TO_TICKS(1));

    // Step 4: Set measurement mode (Mode 1: 6000 LSB/g, 40 Hz)
    ESP_LOGI(TAG, "Step 4: Set Mode 1 (6000 LSB/g, 40 Hz)...");
    ret = scl3300_transfer(SCL3300_CMD_SET_MODE1, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SET_MODE1 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_current_accel_scale = ACCEL_SCALE_MODE1;

    // Step 5: Enable angle outputs via ANG_CTRL
    ESP_LOGI(TAG, "Step 5: Enable angle outputs (ANG_CTRL)...");
    ret = scl3300_transfer(SCL3300_CMD_ANG_CTRL_ENABLE, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ANG_CTRL enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 6: Signal path settling time (25ms for Mode 1)
    ESP_LOGI(TAG, "Step 6: Signal path settling (25ms)...");
    vTaskDelay(pdMS_TO_TICKS(25));

    // Steps 7-9: Read STATUS multiple times to clear due to off-frame protocol
    ESP_LOGI(TAG, "Steps 7-9: Clear STATUS register (3x reads)...");

    ret = scl3300_transfer(SCL3300_CMD_READ_STATUS, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STATUS read 1 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = scl3300_transfer(SCL3300_CMD_READ_STATUS, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STATUS read 2 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = scl3300_transfer(SCL3300_CMD_READ_STATUS, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STATUS read 3 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t rs = scl3300_extract_rs(resp);
    if (rs != SCL3300_RS_NORMAL) {
        ESP_LOGW(TAG, "WARNING: RS bits = 0x%02X (expected 0x01)", rs);
        ESP_LOGW(TAG, "Full STATUS response: 0x%08" PRIX32, resp);

        if (rs == SCL3300_RS_ERROR) {
            ESP_LOGE(TAG, "Startup error detected in STATUS register");
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGI(TAG, "STATUS OK: RS = 0x01 (normal operation)");
    }

    // Optional: WHOAMI verification
    ESP_LOGI(TAG, "Verifying WHOAMI...");

    ret = scl3300_transfer(SCL3300_CMD_READ_WHOAMI, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHOAMI prime failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = scl3300_transfer(SCL3300_CMD_READ_WHOAMI, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHOAMI read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t whoami = (uint16_t)((resp >> 8) & 0xFFFF);
    if (whoami != SCL3300_WHOAMI_VALUE) {
        ESP_LOGW(TAG, "WHOAMI mismatch: 0x%04X (expected 0x%04X)",
                 whoami, (unsigned)SCL3300_WHOAMI_VALUE);
    } else {
        ESP_LOGI(TAG, "WHOAMI OK: 0x%04X", whoami);
    }

    ESP_LOGI(TAG, "SCL3300-D01 initialization complete");
    return ESP_OK;
}

/**
 * @brief Read angle measurements from all three axes
 */
esp_err_t scl3300_read_angle(scl3300_angle_t *angle)
{
    if (angle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t raw_x = 0, raw_y = 0, raw_z = 0;

    esp_err_t ret = scl3300_read_data16(SCL3300_CMD_READ_ANG_X, &raw_x);
    if (ret != ESP_OK) return ret;

    ret = scl3300_read_data16(SCL3300_CMD_READ_ANG_Y, &raw_y);
    if (ret != ESP_OK) return ret;

    ret = scl3300_read_data16(SCL3300_CMD_READ_ANG_Z, &raw_z);
    if (ret != ESP_OK) return ret;

    angle->x = raw_x * (90.0f / 16384.0f);
    angle->y = raw_y * (90.0f / 16384.0f);
    angle->z = raw_z * (90.0f / 16384.0f);

    return ESP_OK;
}

/**
 * @brief Read acceleration measurements from all three axes
 */
esp_err_t scl3300_read_accel(scl3300_accel_t *accel)
{
    if (accel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t raw_x = 0, raw_y = 0, raw_z = 0;

    esp_err_t ret = scl3300_read_data16(SCL3300_CMD_READ_ACC_X, &raw_x);
    if (ret != ESP_OK) return ret;

    ret = scl3300_read_data16(SCL3300_CMD_READ_ACC_Y, &raw_y);
    if (ret != ESP_OK) return ret;

    ret = scl3300_read_data16(SCL3300_CMD_READ_ACC_Z, &raw_z);
    if (ret != ESP_OK) return ret;

    accel->x = raw_x / s_current_accel_scale;
    accel->y = raw_y / s_current_accel_scale;
    accel->z = raw_z / s_current_accel_scale;

    return ESP_OK;
}

/**
 * @brief Enable angle outputs
 */
esp_err_t scl3300_enable_angles(void)
{
    if (s_scl3300 == NULL) {
        ESP_LOGE(TAG, "SCL3300 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t resp = 0;
    esp_err_t ret = scl3300_transfer(SCL3300_CMD_ANG_CTRL_ENABLE, &resp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ANG_CTRL enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Angle outputs enabled");
    return ESP_OK;
}

/**
 * @brief Read and verify the WHOAMI register
 */
esp_err_t scl3300_read_whoami(uint16_t *whoami)
{
    if (whoami == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_scl3300 == NULL) {
        ESP_LOGE(TAG, "SCL3300 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t resp = 0;

    esp_err_t ret = scl3300_transfer(SCL3300_CMD_READ_WHOAMI, &resp);
    if (ret != ESP_OK) return ret;

    ret = scl3300_transfer(SCL3300_CMD_READ_WHOAMI, &resp);
    if (ret != ESP_OK) return ret;

    *whoami = (uint16_t)((resp >> 8) & 0xFFFF);
    return ESP_OK;
}