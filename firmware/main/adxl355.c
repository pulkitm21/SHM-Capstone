/**
 * @file adxl355.c
 * @brief ADXL355 Accelerometer Driver (SPI) - datasheet-correct init + SPI framing
 *
 * Datasheet: ADXL354/ADXL355 Rev. D
 *
 * SPI protocol:
 *  - CPOL=0, CPHA=0
 *  - Command byte:
 *      bit7 = R/W   (1 = read, 0 = write)
 *      bit6 = MB    (1 = multibyte, 0 = single byte)
 *      bit5..0 = register address
 */

#include "adxl355.h"
#include "spi_bus.h"

#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "ADXL355";

spi_device_handle_t adxl355_spi_handle = NULL; //exposed for ISR use
static spi_device_handle_t s_dev = NULL; //internal handle for non-ISR use
static uint8_t s_range_code = ADXL355_RANGE_2G;

/* Small transfers only (IDs, config, accel/temp reads) */
#define ADXL355_MAX_XFER_BYTES  16

/* Soft reset code*/
#define ADXL355_RESET_CODE      0x52

/* ----- SPI helpers ----- */

static inline uint8_t adxl355_cmd(uint8_t reg, bool is_read)
{
    // Datasheet: MOSI sends A6..A0 then R/W as bit0
    // So command byte = (reg << 1) | R/W
    return (uint8_t)((reg << 1) | (is_read ? 0x01 : 0x00));
}

static esp_err_t adxl355_xfer(const uint8_t *tx, uint8_t *rx, size_t nbytes)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!tx || !rx || nbytes == 0) return ESP_ERR_INVALID_ARG;

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    t.length = (uint32_t)(nbytes * 8);
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    return spi_device_transmit(s_dev, &t);
}

static esp_err_t adxl355_read_reg(uint8_t reg, uint8_t *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    if (len + 1 > ADXL355_MAX_XFER_BYTES) return ESP_ERR_INVALID_SIZE;

    uint8_t tx[ADXL355_MAX_XFER_BYTES] = {0};
    uint8_t rx[ADXL355_MAX_XFER_BYTES] = {0};

    tx[0] = adxl355_cmd(reg, true);

    esp_err_t err = adxl355_xfer(tx, rx, 1 + len);
    if (err != ESP_OK) return err;

    memcpy(out, &rx[1], len);
    return ESP_OK;
}

static esp_err_t adxl355_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { adxl355_cmd(reg, false), val };
    uint8_t rx[2] = {0};
    return adxl355_xfer(tx, rx, sizeof(tx));
}

/* ----- Conversions ----- */

static inline int32_t sign_extend_20b(uint32_t v)
{
    /* 20-bit two's complement: sign bit is bit19 */
    if (v & (1u << 19)) {
        return (int32_t)(v | 0xFFF00000u); /* extend sign through bit31 */
    }
    return (int32_t)v;
}

static float adxl355_lsb_per_g(uint8_t range_code)
{
    /* From datasheet sensitivity:
       ±2 g: 3.9 µg/LSB  -> ~256000 LSB/g
       ±4 g: 7.8 µg/LSB  -> ~128000 LSB/g
       ±8 g: 15.6 µg/LSB -> ~ 64000 LSB/g
    */
    switch (range_code) {
        case ADXL355_RANGE_2G: return 256000.0f;
        case ADXL355_RANGE_4G: return 128000.0f;
        case ADXL355_RANGE_8G: return  64000.0f;
        default:               return 256000.0f;
    }
}

/* ----- Public API ----- */

esp_err_t adxl355_init(void)
{
    ESP_LOGI(TAG, "Initializing ADXL355 accelerometer...");

    if (s_dev != NULL) {
        ESP_LOGW(TAG, "ADXL355 already initialized");
        return ESP_OK;
    }

    /* Add device to shared SPI bus */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLOCK_SPEED_HZ,
        .mode = 0, /* CPOL=0, CPHA=0 */
        .spics_io_num = SPI_CS_ADXL355_IO,
        .queue_size = 1,
        .flags = 0, /* full duplex */
    };

    esp_err_t err = spi_bus_add_device(spi_bus_get_host(), &devcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    adxl355_spi_handle = s_dev; //Expose handle for ISR access

    /* Optional soft reset for a clean state */
    (void)adxl355_write_reg(ADXL355_REG_RESET, ADXL355_RESET_CODE);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Verify device IDs */
    uint8_t devid_ad = 0, devid_mst = 0, partid = 0, revid = 0;
    err = adxl355_read_reg(ADXL355_REG_DEVID_AD, &devid_ad, 1);   if (err != ESP_OK) return err;
    err = adxl355_read_reg(ADXL355_REG_DEVID_MST, &devid_mst, 1); if (err != ESP_OK) return err;
    err = adxl355_read_reg(ADXL355_REG_PARTID, &partid, 1);       if (err != ESP_OK) return err;
    err = adxl355_read_reg(ADXL355_REG_REVID, &revid, 1);         if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "IDs: DEVID_AD=0x%02X DEVID_MST=0x%02X PARTID=0x%02X REVID=0x%02X",
             devid_ad, devid_mst, partid, revid);

    if (devid_ad != ADXL355_DEVID_AD_EXPECTED ||
        devid_mst != ADXL355_DEVID_MST_EXPECTED ||
        partid   != ADXL355_PARTID_EXPECTED) {
        ESP_LOGE(TAG, "Unexpected IDs (got AD=0x%02X MST=0x%02X PART=0x%02X; expected AD=0x%02X MST=0x%02X PART=0x%02X)",
                 devid_ad, devid_mst, partid,
                 ADXL355_DEVID_AD_EXPECTED, ADXL355_DEVID_MST_EXPECTED, ADXL355_PARTID_EXPECTED);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Enter standby before changing configuration */
    err = adxl355_write_reg(ADXL355_REG_POWER_CTL, ADXL355_POWER_STANDBY_BIT);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(2));

    /* FILTER: ODR=1000 Hz*/
    err = adxl355_write_reg(ADXL355_REG_FILTER, ADXL355_FILTER_ODR_1000);
    if (err != ESP_OK) return err;

    /* Optional: route DATA_RDY to INT1 via INT_MAP */
    err = adxl355_write_reg(ADXL355_REG_INT_MAP, ADXL355_INT_RDY_EN1);
    if (err != ESP_OK) return err;

    /* RANGE: default ±2 g */
    err = adxl355_set_range(ADXL355_RANGE_2G);
    if (err != ESP_OK) return err;

    /* Exit standby -> measurement mode */
    err = adxl355_write_reg(ADXL355_REG_POWER_CTL, 0x00);
    if (err != ESP_OK) return err;

    /* Cache ranges */
    uint8_t range_reg = 0;
    err = adxl355_read_reg(ADXL355_REG_RANGE, &range_reg, 1);
    if (err == ESP_OK) {
        s_range_code = (uint8_t)(range_reg & 0x03);
    }

    ESP_LOGI(TAG, "ADXL355 init OK (range code=0x%02X)", s_range_code);
    return ESP_OK;
}

esp_err_t adxl355_set_range(uint8_t range)
{
    if (range != ADXL355_RANGE_2G &&
        range != ADXL355_RANGE_4G &&
        range != ADXL355_RANGE_8G) {
        ESP_LOGE(TAG, "Invalid range code: 0x%02X", range);
        return ESP_ERR_INVALID_ARG;
    }

    /* RANGE register reset is 0x81; keep upper bits, set lower 2 bits */
    uint8_t reg = 0;
    esp_err_t err = adxl355_read_reg(ADXL355_REG_RANGE, &reg, 1);
    if (err != ESP_OK) return err;

    reg = (uint8_t)((reg & ~0x03u) | (range & 0x03u));
    err = adxl355_write_reg(ADXL355_REG_RANGE, reg);
    if (err != ESP_OK) return err;

    s_range_code = range;
    ESP_LOGI(TAG, "Range set (code=0x%02X)", s_range_code);
    return ESP_OK;
}

// Reading function is left here for debugging purposes. data_processing_and_mqtt_task.c converts raw values to g's.
// esp_err_t adxl355_read_acceleration(adxl355_accel_t *accel)
// {
//     if (!accel) return ESP_ERR_INVALID_ARG;

//     uint8_t b[9] = {0};
//     esp_err_t err = adxl355_read_reg(ADXL355_REG_XDATA3, b, sizeof(b));
//     if (err != ESP_OK) return err;

//     uint32_t x_u = ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | ((uint32_t)b[2] >> 4);
//     uint32_t y_u = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | ((uint32_t)b[5] >> 4);
//     uint32_t z_u = ((uint32_t)b[6] << 12) | ((uint32_t)b[7] << 4) | ((uint32_t)b[8] >> 4);

//     int32_t x = sign_extend_20b(x_u);
//     int32_t y = sign_extend_20b(y_u);
//     int32_t z = sign_extend_20b(z_u);

//     float lsb_per_g = adxl355_lsb_per_g(s_range_code);

//     accel->x = (float)x / lsb_per_g;
//     accel->y = (float)y / lsb_per_g;
//     accel->z = (float)z / lsb_per_g;

//     return ESP_OK;
// }

esp_err_t adxl355_read_temperature(float *temperature_c)
{
    if (!temperature_c) return ESP_ERR_INVALID_ARG;

    /* TEMP is not double-buffered. Read TEMP2, TEMP1, TEMP2 and verify TEMP2 stable. */
    uint8_t t2a = 0, t1 = 0, t2b = 0;

    for (int i = 0; i < 3; i++) {
        esp_err_t err = adxl355_read_reg(ADXL355_REG_TEMP2, &t2a, 1);
        if (err != ESP_OK) return err;
        err = adxl355_read_reg(ADXL355_REG_TEMP1, &t1, 1);
        if (err != ESP_OK) return err;
        err = adxl355_read_reg(ADXL355_REG_TEMP2, &t2b, 1);
        if (err != ESP_OK) return err;

        if ((t2a & 0x0F) == (t2b & 0x0F)) break;
    }

    uint16_t raw12 = (uint16_t)(((t2b & 0x0F) << 8) | t1);

    /* Datasheet: nominal intercept = 1885 LSB @ 25°C, nominal slope = -9.05 LSB/°C */
    *temperature_c = 25.0f + ((float)raw12 - 1885.0f) / (-9.05f);

    return ESP_OK;
}