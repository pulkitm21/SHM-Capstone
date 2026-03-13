/**
 * @file sensor_task.c
 * @brief ISR-Based Sensor Data Acquisition with Ring Buffers
 *
 * Design Philosophy:
 * ================================================
 * 1. SINGLE ISR driven by GPTimer frequency (8000 Hz)
 * 2. Staggered sensor sampling to prevent conflicts
 * 3. Raw data collection ONLY in ISR
 * 4. Ring buffers provide lock-free handoff to processing tasks
 * 5. Processing happens OUTSIDE the ISR
 *
 * Sensor Configuration:
 * =====================
 * - ADXL355: 1000 Hz (samples every 8 ticks)
 * - SCL3300: 20 Hz   (samples every 400 ticks)
 * - ADT7420: 1 Hz    (NOT read in ISR; left commented out)
 *
 * CS ownership model:
 * ===================
 * - ADXL355: automatic CS handled by SPI device config
 * - SCL3300: manual CS handled explicitly
 *
 * Important:
 * ==========
 * - Do NOT manually toggle ADXL355 CS in ISR
 * - Do keep SCL3300 CS manual and explicit
 * - Temperature ISR block remains commented out
 */

#include "sensor_task.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "hal/i2c_hal.h"
#include "hal/i2c_ll.h"
#include "soc/i2c_struct.h"

#include "spi_bus.h"
#include "i2c_bus.h"
#include "adt7420.h"
#include "adxl355.h"
#include "scl3300.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "sensor_task";

/******************************************************************************
 * CONFIGURATION
 *****************************************************************************/

#define BASE_TIMER_FREQ_HZ      8000
#define TIMER_PERIOD_US         125

#define ADXL355_RATE_HZ         1000
#define SCL3300_RATE_HZ         20
#define ADT7420_RATE_HZ         1

#define ADXL355_TICK_DIVISOR    (BASE_TIMER_FREQ_HZ / ADXL355_RATE_HZ)
#define SCL3300_TICK_DIVISOR    (BASE_TIMER_FREQ_HZ / SCL3300_RATE_HZ)
#define ADT7420_TICK_DIVISOR    (BASE_TIMER_FREQ_HZ / ADT7420_RATE_HZ)

#define ADXL355_OFFSET          0
#define SCL3300_OFFSET          1
#define ADT7420_OFFSET          2

#define ADXL355_BUFFER_SIZE     4096
#define SCL3300_BUFFER_SIZE     128
#define ADT7420_BUFFER_SIZE     16

/******************************************************************************
 * SCL3300 COMMANDS
 *****************************************************************************/

#define SCL3300_CMD_X           0x040000F7u
#define SCL3300_CMD_Y           0x080000FDu
#define SCL3300_CMD_Z           0x0C0000FBu

/******************************************************************************
 * DATA STRUCTURES
 *****************************************************************************/

typedef struct {
    adxl355_raw_sample_t buffer[ADXL355_BUFFER_SIZE];
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t overflow_count;
} adxl355_ring_buffer_t;

typedef struct {
    scl3300_raw_sample_t buffer[SCL3300_BUFFER_SIZE];
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t overflow_count;
} scl3300_ring_buffer_t;

typedef struct {
    adt7420_raw_sample_t buffer[ADT7420_BUFFER_SIZE];
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t overflow_count;
} adt7420_ring_buffer_t;

/******************************************************************************
 * GLOBAL VARIABLES
 *****************************************************************************/

static adxl355_ring_buffer_t adxl355_ring_buffer = {0};
static scl3300_ring_buffer_t scl3300_ring_buffer = {0};
static adt7420_ring_buffer_t adt7420_ring_buffer = {0};

static gptimer_handle_t s_timer = NULL;

static volatile uint32_t tick_counter = 0;

static volatile uint32_t adxl355_sample_count = 0;
static volatile uint32_t scl3300_sample_count = 0;
static volatile uint32_t adt7420_sample_count = 0;

static bool s_temp_available = false;

extern spi_device_handle_t adxl355_spi_handle;
extern spi_device_handle_t scl3300_spi_handle;
extern i2c_master_dev_handle_t adt7420_i2c_handle;

/* SCL3300 rolling pipeline state */
static bool s_scl_pipeline_primed = false;
static bool s_scl_discard_first_sample = true;

/******************************************************************************
 * ACCESS FUNCTIONS FOR ISR
 *****************************************************************************/

static inline void IRAM_ATTR read_adxl355_raw(int32_t *raw_x, int32_t *raw_y, int32_t *raw_z)
{
    uint8_t tx[10] = {0};
    uint8_t rx[10] = {0};

    tx[0] = (0x08 << 1) | 0x01;  // XDATA3 burst read

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 10 * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

#ifdef SPI_CS_SCL3300_IO
    /* Force the OTHER SPI device inactive. */
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
#endif

    /* Do NOT manually drive ADXL355 CS here.
       ADXL355 uses automatic CS in its driver config. */
    spi_device_polling_transmit(adxl355_spi_handle, &t);

    uint32_t x_u = ((uint32_t)rx[1] << 12) | ((uint32_t)rx[2] << 4) | ((uint32_t)rx[3] >> 4);
    uint32_t y_u = ((uint32_t)rx[4] << 12) | ((uint32_t)rx[5] << 4) | ((uint32_t)rx[6] >> 4);
    uint32_t z_u = ((uint32_t)rx[7] << 12) | ((uint32_t)rx[8] << 4) | ((uint32_t)rx[9] >> 4);

    *raw_x = (x_u & 0x80000u) ? (int32_t)(x_u | 0xFFF00000u) : (int32_t)x_u;
    *raw_y = (y_u & 0x80000u) ? (int32_t)(y_u | 0xFFF00000u) : (int32_t)y_u;
    *raw_z = (z_u & 0x80000u) ? (int32_t)(z_u | 0xFFF00000u) : (int32_t)z_u;
}

static inline void IRAM_ATTR scl3300_isr_transfer(uint32_t cmd, uint32_t *resp_out)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 32;
    t.rxlength = 32;

    t.tx_data[0] = (uint8_t)((cmd >> 24) & 0xFF);
    t.tx_data[1] = (uint8_t)((cmd >> 16) & 0xFF);
    t.tx_data[2] = (uint8_t)((cmd >> 8) & 0xFF);
    t.tx_data[3] = (uint8_t)(cmd & 0xFF);

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
    gpio_set_level(SPI_CS_SCL3300_IO, 0);
#endif

    spi_device_polling_transmit(scl3300_spi_handle, &t);

#ifdef SPI_CS_SCL3300_IO
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
#endif

    if (resp_out) {
        *resp_out = ((uint32_t)t.rx_data[0] << 24) |
                    ((uint32_t)t.rx_data[1] << 16) |
                    ((uint32_t)t.rx_data[2] << 8)  |
                    (uint32_t)t.rx_data[3];
    }
}

static inline int16_t IRAM_ATTR scl3300_unpack_raw16(uint32_t resp)
{
    return (int16_t)((resp >> 8) & 0xFFFFu);
}

static inline void IRAM_ATTR scl3300_prime_pipeline_once(void)
{
    uint32_t dummy;
    scl3300_isr_transfer(SCL3300_CMD_X, &dummy);
    s_scl_pipeline_primed = true;
    s_scl_discard_first_sample = true;
}

static inline bool IRAM_ATTR read_scl3300_raw(int16_t *raw_x, int16_t *raw_y, int16_t *raw_z)
{
    uint32_t resp_x;
    uint32_t resp_y;
    uint32_t resp_z;

    if (!s_scl_pipeline_primed) {
        scl3300_prime_pipeline_once();
        return false;
    }

    /* Rolling off-frame sequence:
       send Y -> receive X
       send Z -> receive Y
       send X -> receive Z */
    scl3300_isr_transfer(SCL3300_CMD_Y, &resp_x);
    scl3300_isr_transfer(SCL3300_CMD_Z, &resp_y);
    scl3300_isr_transfer(SCL3300_CMD_X, &resp_z);

    *raw_x = scl3300_unpack_raw16(resp_x);
    *raw_y = scl3300_unpack_raw16(resp_y);
    *raw_z = scl3300_unpack_raw16(resp_z);

    if (s_scl_discard_first_sample) {
        s_scl_discard_first_sample = false;
        return false;
    }

    return true;
}

static inline void IRAM_ATTR read_adt7420_raw(uint16_t *raw_temp)
{
    /* Intentionally unused in ISR */
    *raw_temp = 0;
}

/******************************************************************************
 * ISR
 *****************************************************************************/

static bool IRAM_ATTR timer_isr_handler(gptimer_handle_t timer,
                                        const gptimer_alarm_event_data_t *edata,
                                        void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    tick_counter++;

    uint32_t next_write_index;
    uint32_t current_read_index;

    /* ADXL355 @ 1000 Hz */
    if (((tick_counter - ADXL355_OFFSET) & (ADXL355_TICK_DIVISOR - 1u)) == 0u)
    {
        next_write_index = (adxl355_ring_buffer.write_index + 1u) & (ADXL355_BUFFER_SIZE - 1u);
        current_read_index = adxl355_ring_buffer.read_index;

        if (next_write_index == current_read_index)
        {
            adxl355_ring_buffer.overflow_count++;
        }
        else
        {
            adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].tick = tick_counter;

            read_adxl355_raw(
                &adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].raw_x,
                &adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].raw_y,
                &adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].raw_z
            );

            adxl355_ring_buffer.write_index = next_write_index;
            adxl355_sample_count++;
        }
    }

    /* SCL3300 @ 20 Hz */
    if (((tick_counter - SCL3300_OFFSET) % SCL3300_TICK_DIVISOR) == 0u)
    {
        int16_t raw_x = 0;
        int16_t raw_y = 0;
        int16_t raw_z = 0;

        next_write_index = (scl3300_ring_buffer.write_index + 1u) & (SCL3300_BUFFER_SIZE - 1u);
        current_read_index = scl3300_ring_buffer.read_index;

        if (next_write_index == current_read_index)
        {
            scl3300_ring_buffer.overflow_count++;
        }
        else
        {
            bool valid = read_scl3300_raw(&raw_x, &raw_y, &raw_z);

            if (valid)
            {
                scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].tick = tick_counter;
                scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].raw_x = raw_x;
                scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].raw_y = raw_y;
                scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].raw_z = raw_z;

                scl3300_ring_buffer.write_index = next_write_index;
                scl3300_sample_count++;
            }
        }
    }

    /*
    // ADT7420 Sampling (1 Hz - every 8000 ticks)
    // Intentionally left commented out.
    if (s_temp_available && ((tick_counter - ADT7420_OFFSET) % ADT7420_TICK_DIVISOR) == 0u)
    {
        next_write_index = (adt7420_ring_buffer.write_index + 1u) & (ADT7420_BUFFER_SIZE - 1u);
        current_read_index = adt7420_ring_buffer.read_index;

        if (next_write_index == current_read_index)
        {
            adt7420_ring_buffer.overflow_count++;
        }
        else
        {
            adt7420_ring_buffer.buffer[adt7420_ring_buffer.write_index].tick = tick_counter;
            read_adt7420_raw(&adt7420_ring_buffer.buffer[adt7420_ring_buffer.write_index].raw_temp);
            adt7420_ring_buffer.write_index = next_write_index;
            adt7420_sample_count++;
        }
    }
    */

    return false;
}

/******************************************************************************
 * INITIALIZATION
 *****************************************************************************/

esp_err_t sensor_acquisition_init(bool temp_sensor_available)
{
    esp_err_t ret;

    s_temp_available = temp_sensor_available;

    ESP_LOGI(TAG, "Initializing ISR-based sensor acquisition...");
    ESP_LOGI(TAG, "  Base timer: %d Hz (%d us period)", BASE_TIMER_FREQ_HZ, TIMER_PERIOD_US);
    ESP_LOGI(TAG, "  ADXL355: %d Hz (every %d ticks, offset %d)",
             ADXL355_RATE_HZ, ADXL355_TICK_DIVISOR, ADXL355_OFFSET);
    ESP_LOGI(TAG, "  SCL3300: %d Hz (every %d ticks, offset %d)",
             SCL3300_RATE_HZ, SCL3300_TICK_DIVISOR, SCL3300_OFFSET);
    ESP_LOGI(TAG, "  ADT7420: %d Hz (every %d ticks, offset %d)",
             ADT7420_RATE_HZ, ADT7420_TICK_DIVISOR, ADT7420_OFFSET);

    memset(&adxl355_ring_buffer, 0, sizeof(adxl355_ring_buffer));
    memset(&scl3300_ring_buffer, 0, sizeof(scl3300_ring_buffer));
    memset(&adt7420_ring_buffer, 0, sizeof(adt7420_ring_buffer));

    tick_counter = 0;
    adxl355_sample_count = 0;
    scl3300_sample_count = 0;
    adt7420_sample_count = 0;

    s_scl_pipeline_primed = false;
    s_scl_discard_first_sample = true;

#ifdef SPI_CS_SCL3300_IO
    gpio_set_direction(SPI_CS_SCL3300_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(SPI_CS_SCL3300_IO, 1);
#endif

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };

    ret = gptimer_new_timer(&timer_config, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_isr_handler,
    };
    ret = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register timer callback: %s", esp_err_to_name(ret));
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        return ret;
    }

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = TIMER_PERIOD_US,
        .flags.auto_reload_on_alarm = true,
    };
    ret = gptimer_set_alarm_action(s_timer, &alarm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer alarm: %s", esp_err_to_name(ret));
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        return ret;
    }

    ret = gptimer_enable(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer: %s", esp_err_to_name(ret));
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Sensor acquisition initialized successfully");
    ESP_LOGI(TAG, "Ring buffer sizes: ADXL=%d, SCL=%d, ADT=%d",
             ADXL355_BUFFER_SIZE, SCL3300_BUFFER_SIZE, ADT7420_BUFFER_SIZE);

    return ESP_OK;
}

esp_err_t sensor_acquisition_start(void)
{
    if (s_timer == NULL) {
        ESP_LOGE(TAG, "Timer not initialized");
        return ESP_FAIL;
    }

    sensor_acquisition_reset_stats();

    esp_err_t ret = gptimer_start(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Sensor acquisition STARTED");
    return ESP_OK;
}

esp_err_t sensor_acquisition_stop(void)
{
    if (s_timer == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = gptimer_stop(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Sensor acquisition STOPPED");
    return ESP_OK;
}

/******************************************************************************
 * RING BUFFER ACCESS FUNCTIONS
 *****************************************************************************/

bool adxl355_data_available(void)
{
    return (adxl355_ring_buffer.write_index != adxl355_ring_buffer.read_index);
}

bool adxl355_read_sample(adxl355_raw_sample_t *sample)
{
    if (!adxl355_data_available())
    {
        return false;
    }

    *sample = adxl355_ring_buffer.buffer[adxl355_ring_buffer.read_index];
    adxl355_ring_buffer.read_index =
        (adxl355_ring_buffer.read_index + 1u) & (ADXL355_BUFFER_SIZE - 1u);

    return true;
}

uint32_t adxl355_samples_available(void)
{
    uint32_t write = adxl355_ring_buffer.write_index;
    uint32_t read = adxl355_ring_buffer.read_index;

    if (write >= read)
    {
        return write - read;
    }
    else
    {
        return ADXL355_BUFFER_SIZE - read + write;
    }
}

bool scl3300_data_available(void)
{
    return (scl3300_ring_buffer.write_index != scl3300_ring_buffer.read_index);
}

bool scl3300_read_sample(scl3300_raw_sample_t *sample)
{
    if (!scl3300_data_available())
    {
        return false;
    }

    *sample = scl3300_ring_buffer.buffer[scl3300_ring_buffer.read_index];
    scl3300_ring_buffer.read_index =
        (scl3300_ring_buffer.read_index + 1u) & (SCL3300_BUFFER_SIZE - 1u);

    return true;
}

uint32_t scl3300_samples_available(void)
{
    uint32_t write = scl3300_ring_buffer.write_index;
    uint32_t read = scl3300_ring_buffer.read_index;

    if (write >= read)
    {
        return write - read;
    }
    else
    {
        return SCL3300_BUFFER_SIZE - read + write;
    }
}

bool adt7420_data_available(void)
{
    return (adt7420_ring_buffer.write_index != adt7420_ring_buffer.read_index);
}

bool adt7420_read_sample(adt7420_raw_sample_t *sample)
{
    if (!adt7420_data_available())
    {
        return false;
    }

    *sample = adt7420_ring_buffer.buffer[adt7420_ring_buffer.read_index];
    adt7420_ring_buffer.read_index =
        (adt7420_ring_buffer.read_index + 1u) & (ADT7420_BUFFER_SIZE - 1u);

    return true;
}

uint32_t adt7420_samples_available(void)
{
    uint32_t write = adt7420_ring_buffer.write_index;
    uint32_t read = adt7420_ring_buffer.read_index;

    if (write >= read)
    {
        return write - read;
    }
    else
    {
        return ADT7420_BUFFER_SIZE - read + write;
    }
}

/******************************************************************************
 * DIAGNOSTICS
 *****************************************************************************/

void sensor_acquisition_get_stats(uint32_t *samples_acquired,
                                  uint32_t *samples_dropped,
                                  uint32_t *max_acquisition_time_us)
{
    if (samples_acquired) {
        *samples_acquired = adxl355_sample_count + scl3300_sample_count + adt7420_sample_count;
    }
    if (samples_dropped) {
        *samples_dropped = adxl355_ring_buffer.overflow_count +
                           scl3300_ring_buffer.overflow_count +
                           adt7420_ring_buffer.overflow_count;
    }
    if (max_acquisition_time_us) {
        *max_acquisition_time_us = 0;
    }
}

void sensor_acquisition_reset_stats(void)
{
    adxl355_sample_count = 0;
    scl3300_sample_count = 0;
    adt7420_sample_count = 0;

    adxl355_ring_buffer.overflow_count = 0;
    scl3300_ring_buffer.overflow_count = 0;
    adt7420_ring_buffer.overflow_count = 0;

    tick_counter = 0;

    s_scl_pipeline_primed = false;
    s_scl_discard_first_sample = true;
}

uint32_t adxl355_get_overflow_count(void)
{
    return adxl355_ring_buffer.overflow_count;
}

uint32_t scl3300_get_overflow_count(void)
{
    return scl3300_ring_buffer.overflow_count;
}

uint32_t adt7420_get_overflow_count(void)
{
    return adt7420_ring_buffer.overflow_count;
}

uint32_t adxl355_get_sample_count(void)
{
    return adxl355_sample_count;
}

uint32_t scl3300_get_sample_count(void)
{
    return scl3300_sample_count;
}

uint32_t adt7420_get_sample_count(void)
{
    return adt7420_sample_count;
}

uint32_t get_tick_count(void)
{
    return tick_counter;
}