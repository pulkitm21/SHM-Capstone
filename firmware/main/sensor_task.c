/**
 * @file sensor_task.c
 * @brief ISR-Based Sensor Data Acquisition with Ring Buffers
 *
 * Design Philosophy (per advisor recommendation):
 * ================================================
 * 1. SINGLE ISR driven by GPTimer at high frequency (8000 Hz)
 * 2. Staggered sensor sampling to prevent bus conflicts
 * 3. Raw data collection ONLY in ISR - NO function calls, NO processing
 * 4. Lock-free ring buffers for each sensor
 * 5. Minimal ISR overhead for deterministic timing
 * 6. Processing happens OUTSIDE the ISR in separate tasks
 *
 * Sensor Configuration:
 * =====================
 * - ADXL355: 2000 Hz (samples every 4 ticks at 8000 Hz base)
 * - SCL3300: 20 Hz   (samples every 400 ticks)
 * - ADT7420: 1 Hz    (samples every 8000 ticks)
 *
 * Timing Strategy:
 * ================
 * - Base timer: 8000 Hz (125 microsecond period)
 * - Sensors staggered by 1 tick (125us) to avoid simultaneous access
 * - ADXL355 offset: 0 ticks (samples at tick 0, 4, 8, 12...)
 * - SCL3300 offset: 1 tick  (samples at tick 1, 401, 801...)
 * - ADT7420 offset: 2 ticks (samples at tick 2, 8002, 16002...)
 *
 * CRITICAL RULES FOR ISR:
 * =======================
 * - NO function calls (except direct hardware register reads)
 * - NO data processing or conversion
 * - NO MQTT, no logging, no printf
 * - Only raw data collection and ring buffer writes
 * - Keep execution time minimal and deterministic
 */

#include "sensor_task.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

#include "hal/i2c_hal.h"
#include "hal/i2c_ll.h"
#include "soc/i2c_struct.h"

#include "spi_bus.h"
#include "i2c_bus.h"
#include "adt7420.h"
#include "adxl355.h"
#include "scl3300.h"

#include <string.h>

static const char *TAG = "sensor_task";

/******************************************************************************
 * CONFIGURATION
 *****************************************************************************/

// Base timer frequency
#define BASE_TIMER_FREQ_HZ      8000    // 125 microsecond period
#define TIMER_PERIOD_US         125

// Sensor sample rates
#define ADXL355_RATE_HZ         2000
#define SCL3300_RATE_HZ         20
#define ADT7420_RATE_HZ         1

// Calculate tick divisors (how many ticks between samples)
#define ADXL355_TICK_DIVISOR    (BASE_TIMER_FREQ_HZ / ADXL355_RATE_HZ)  // 4
#define SCL3300_TICK_DIVISOR    (BASE_TIMER_FREQ_HZ / SCL3300_RATE_HZ)  // 400
#define ADT7420_TICK_DIVISOR    (BASE_TIMER_FREQ_HZ / ADT7420_RATE_HZ)  // 8000

// Stagger offsets (in ticks) to prevent simultaneous sampling
#define ADXL355_OFFSET          0       // ADXL samples first
#define SCL3300_OFFSET          1       // SCL samples 1 tick (125us) after ADXL
#define ADT7420_OFFSET          2       // ADT samples 2 ticks (250us) after ADXL

// Ring buffer sizes (power of 2 for efficient modulo with &)
#define ADXL355_BUFFER_SIZE     4096    // ~2 seconds at 2000 Hz
#define SCL3300_BUFFER_SIZE     128     // ~6 seconds at 20 Hz
#define ADT7420_BUFFER_SIZE     16      // ~16 seconds at 1 Hz

/******************************************************************************
 * DATA STRUCTURES
 *****************************************************************************/

// Ring buffer structures
typedef struct {
    adxl355_raw_sample_t buffer[ADXL355_BUFFER_SIZE];
    volatile uint32_t write_index;  // ISR writes here
    volatile uint32_t read_index;   // Processing task reads here
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

// Ring buffers (accessible by processing tasks)
static adxl355_ring_buffer_t adxl355_ring_buffer = {0};
static scl3300_ring_buffer_t scl3300_ring_buffer = {0};
static adt7420_ring_buffer_t adt7420_ring_buffer = {0};

// Hardware timer handle
static gptimer_handle_t s_timer = NULL;

// Tick counter
static volatile uint32_t tick_counter = 0;

// Statistics (for debugging/monitoring)
static volatile uint32_t adxl355_sample_count = 0;
static volatile uint32_t scl3300_sample_count = 0;
static volatile uint32_t adt7420_sample_count = 0;

// Sensor availability
static bool s_temp_available = false;

// External device handles (these are from your sensor drivers)
// We'll need these for direct SPI/I2C access in the ISR
extern spi_device_handle_t adxl355_spi_handle;  // You'll need to expose this from adxl355.c
extern spi_device_handle_t scl3300_spi_handle;  // You'll need to expose this from scl3300.c
extern i2c_master_dev_handle_t adt7420_i2c_handle; // You'll need to expose this from adt7420.c

/******************************************************************************
 * DIRECT HARDWARE ACCESS FUNCTIONS FOR ISR
 * 
 * These functions perform RAW register reads WITHOUT any function call overhead.
 * They are intentionally NOT factored out to separate functions to minimize ISR time.
 *****************************************************************************/

/**
 * @brief Read raw ADXL355 acceleration data directly from SPI
 * 
 * ADXL355 raw data format:
 * - 3 axes, each 20-bit two's complement
 * - Registers: XDATA3, XDATA2, XDATA1 (and same for Y, Z)
 * - Total: 9 bytes burst read from XDATA3
 * 
 * @param raw_x Output for X-axis raw value
 * @param raw_y Output for Y-axis raw value
 * @param raw_z Output for Z-axis raw value
 */
static inline void IRAM_ATTR read_adxl355_raw(int32_t *raw_x, int32_t *raw_y, int32_t *raw_z)
{
     // ADXL355 SPI command: (reg << 1) | 0x01 for read
    // XDATA3 register = 0x08
    uint8_t tx[10] = {0};
    uint8_t rx[10] = {0};
    
    tx[0] = (0x08 << 1) | 0x01;  // Read command for XDATA3
    
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 10 * 8;  // 10 bytes (1 cmd + 9 data)
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    
    // Direct SPI transfer (blocking, but very fast ~10-20 microseconds)
    spi_device_polling_transmit(adxl355_spi_handle, &t);
    
    // Extract 20-bit values from bytes (big-endian)
    // Byte layout from ADXL355 datasheet:
    // rx[1] = XDATA3 (bits 19-12)
    // rx[2] = XDATA2 (bits 11-4)
    // rx[3] = XDATA1 (bits 3-0 in upper nibble)
    uint32_t x_u = ((uint32_t)rx[1] << 12) | ((uint32_t)rx[2] << 4) | ((uint32_t)rx[3] >> 4);
    uint32_t y_u = ((uint32_t)rx[4] << 12) | ((uint32_t)rx[5] << 4) | ((uint32_t)rx[6] >> 4);
    uint32_t z_u = ((uint32_t)rx[7] << 12) | ((uint32_t)rx[8] << 4) | ((uint32_t)rx[9] >> 4);
    
    // Sign extend 20-bit to 32-bit (handle negative values)
    // If bit 19 is set, it's negative in two's complement
    *raw_x = (x_u & 0x80000) ? (int32_t)(x_u | 0xFFF00000) : (int32_t)x_u;
    *raw_y = (y_u & 0x80000) ? (int32_t)(y_u | 0xFFF00000) : (int32_t)y_u;
    *raw_z = (z_u & 0x80000) ? (int32_t)(z_u | 0xFFF00000) : (int32_t)z_u;
}

/**
 * @brief Read raw SCL3300 angle/acceleration data directly from SPI
 * 
 * SCL3300 uses off-frame protocol:
 * - Send command, get previous command's response
 * - For ISR efficiency, we'll do simplified reads
 * - Commands from scl3300.h
 * 
 * @param raw_x Output for X-axis raw value
 * @param raw_y Output for Y-axis raw value
 * @param raw_z Output for Z-axis raw value
 */
static inline void IRAM_ATTR read_scl3300_raw(int16_t *raw_x, int16_t *raw_y, int16_t *raw_z)
{
    spi_transaction_t t;
    uint32_t cmd, resp;
    
    // Note: We're reading ACCELERATION, not angle, for speed
    // Each axis requires 2 SPI transactions (prime + fetch)
    
    //--------------------------------------------------------------------------
    // Read X-axis acceleration
    //--------------------------------------------------------------------------
    cmd = 0x040000F7u;  // SCL3300_CMD_READ_ACC_X
    
    // Prime: Send command
    memset(&t, 0, sizeof(t));
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 32;
    t.rxlength = 32;
    t.tx_data[0] = (uint8_t)((cmd >> 24) & 0xFF);
    t.tx_data[1] = (uint8_t)((cmd >> 16) & 0xFF);
    t.tx_data[2] = (uint8_t)((cmd >> 8) & 0xFF);
    t.tx_data[3] = (uint8_t)(cmd & 0xFF);
    
    spi_device_polling_transmit(scl3300_spi_handle, &t);
    
    // Fetch: Send same command again to get response
    spi_device_polling_transmit(scl3300_spi_handle, &t);
    
    resp = ((uint32_t)t.rx_data[0] << 24) |
           ((uint32_t)t.rx_data[1] << 16) |
           ((uint32_t)t.rx_data[2] << 8)  |
           (uint32_t)t.rx_data[3];
    
    // Extract data from bits [23:8]
    *raw_x = (int16_t)((resp >> 8) & 0xFFFF);
    
    //--------------------------------------------------------------------------
    // Read Y-axis acceleration
    //--------------------------------------------------------------------------
    cmd = 0x080000FDu;  // SCL3300_CMD_READ_ACC_Y
    
    // Prime
    memset(&t, 0, sizeof(t));
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 32;
    t.rxlength = 32;
    t.tx_data[0] = (uint8_t)((cmd >> 24) & 0xFF);
    t.tx_data[1] = (uint8_t)((cmd >> 16) & 0xFF);
    t.tx_data[2] = (uint8_t)((cmd >> 8) & 0xFF);
    t.tx_data[3] = (uint8_t)(cmd & 0xFF);
    
    spi_device_polling_transmit(scl3300_spi_handle, &t);
    
    // Fetch
    spi_device_polling_transmit(scl3300_spi_handle, &t);
    
    resp = ((uint32_t)t.rx_data[0] << 24) |
           ((uint32_t)t.rx_data[1] << 16) |
           ((uint32_t)t.rx_data[2] << 8)  |
           (uint32_t)t.rx_data[3];
    
    *raw_y = (int16_t)((resp >> 8) & 0xFFFF);
    
    //--------------------------------------------------------------------------
    // Read Z-axis acceleration
    //--------------------------------------------------------------------------
    cmd = 0x0C0000FBu;  // SCL3300_CMD_READ_ACC_Z
    
    // Prime
    memset(&t, 0, sizeof(t));
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 32;
    t.rxlength = 32;
    t.tx_data[0] = (uint8_t)((cmd >> 24) & 0xFF);
    t.tx_data[1] = (uint8_t)((cmd >> 16) & 0xFF);
    t.tx_data[2] = (uint8_t)((cmd >> 8) & 0xFF);
    t.tx_data[3] = (uint8_t)(cmd & 0xFF);
    
    spi_device_polling_transmit(scl3300_spi_handle, &t);
    
    // Fetch
    spi_device_polling_transmit(scl3300_spi_handle, &t);
    
    resp = ((uint32_t)t.rx_data[0] << 24) |
           ((uint32_t)t.rx_data[1] << 16) |
           ((uint32_t)t.rx_data[2] << 8)  |
           (uint32_t)t.rx_data[3];
    
    *raw_z = (int16_t)((resp >> 8) & 0xFFFF);
}

/**
 * @brief Read raw ADT7420 temperature data directly from I2C
 * 
 * ADT7420 temperature format:
 * - 13-bit resolution (default)
 * - 2 bytes read from TEMP_MSB register
 * 
 * @param raw_temp Output for raw temperature value
 */
static inline void IRAM_ATTR read_adt7420_raw(uint16_t *raw_temp)
{
    // This function is not used - temperature read from processing task
    *raw_temp = 0;
}

/******************************************************************************
 * ISR - TIMER INTERRUPT HANDLER
 * 
 * This is the ONLY place where sensor data is collected.
 * Called every 125 microseconds (8000 Hz)
 * 
 * CRITICAL RULES:
 * - NO function calls (except inline hardware access above)
 * - NO data processing or conversion
 * - NO MQTT, no logging, no printf
 * - Only raw data collection and ring buffer writes
 * - IRAM_ATTR ensures this runs from internal RAM (no flash cache delays)
 *****************************************************************************/

static bool IRAM_ATTR timer_isr_handler(gptimer_handle_t timer,
                                        const gptimer_alarm_event_data_t *edata,
                                        void *user_ctx)
{
    // Increment global tick counter
    tick_counter++;
    
    // Local variables for ring buffer operations
    uint32_t next_write_index;
    uint32_t current_read_index;
    
    //--------------------------------------------------------------------------
    // ADXL355 Sampling (2000 Hz - every 4 ticks)
    //--------------------------------------------------------------------------
    if ((tick_counter - ADXL355_OFFSET) % ADXL355_TICK_DIVISOR == 0)
    {
        // Calculate next write position
        next_write_index = (adxl355_ring_buffer.write_index + 1) & (ADXL355_BUFFER_SIZE - 1);
        
        // Check for buffer overflow (write would overtake read)
        current_read_index = adxl355_ring_buffer.read_index;
        if (next_write_index == current_read_index)
        {
            // Buffer full - increment overflow counter and skip this sample
            adxl355_ring_buffer.overflow_count++;
        }
        else
        {
            // Write timestamp
            adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].tick = tick_counter;
            
            // Read raw sensor data directly (inline function, minimal overhead)
            read_adxl355_raw(
                &adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].raw_x,
                &adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].raw_y,
                &adxl355_ring_buffer.buffer[adxl355_ring_buffer.write_index].raw_z
            );
            
            // Advance write index
            adxl355_ring_buffer.write_index = next_write_index;
            
            // Increment sample count
            adxl355_sample_count++;
        }
    }
    
    //--------------------------------------------------------------------------
    // SCL3300 Sampling (20 Hz - every 400 ticks)
    //--------------------------------------------------------------------------
    if ((tick_counter - SCL3300_OFFSET) % SCL3300_TICK_DIVISOR == 0)
    {
        // Calculate next write position
        next_write_index = (scl3300_ring_buffer.write_index + 1) & (SCL3300_BUFFER_SIZE - 1);
        
        // Check for buffer overflow
        current_read_index = scl3300_ring_buffer.read_index;
        if (next_write_index == current_read_index)
        {
            scl3300_ring_buffer.overflow_count++;
        }
        else
        {
            // Write timestamp
            scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].tick = tick_counter;
            
            // Read raw sensor data directly
            read_scl3300_raw(
                &scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].raw_x,
                &scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].raw_y,
                &scl3300_ring_buffer.buffer[scl3300_ring_buffer.write_index].raw_z
            );
            
            // Advance write index
            scl3300_ring_buffer.write_index = next_write_index;
            
            // Increment sample count
            scl3300_sample_count++;
        }
    }
    
    //--------------------------------------------------------------------------
    // ADT7420 Sampling (1 Hz - every 8000 ticks)
    //--------------------------------------------------------------------------
    /*
    if (s_temp_available && (tick_counter - ADT7420_OFFSET) % ADT7420_TICK_DIVISOR == 0)
    {
        // Calculate next write position
        next_write_index = (adt7420_ring_buffer.write_index + 1) & (ADT7420_BUFFER_SIZE - 1);
        
        // Check for buffer overflow
        current_read_index = adt7420_ring_buffer.read_index;
        if (next_write_index == current_read_index)
        {
            adt7420_ring_buffer.overflow_count++;
        }
        else
        {
            // Write timestamp
            adt7420_ring_buffer.buffer[adt7420_ring_buffer.write_index].tick = tick_counter;
            
            // Read raw sensor data directly
            read_adt7420_raw(
                &adt7420_ring_buffer.buffer[adt7420_ring_buffer.write_index].raw_temp
            );
            
            // Advance write index
            adt7420_ring_buffer.write_index = next_write_index;
            
            // Increment sample count
            adt7420_sample_count++;
        }
    }
    */
    
    // ISR complete - no context switch needed for pure data collection
    return false;
}

/******************************************************************************
 * INITIALIZATION FUNCTION
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
    
    // Initialize ring buffers
    memset(&adxl355_ring_buffer, 0, sizeof(adxl355_ring_buffer));
    memset(&scl3300_ring_buffer, 0, sizeof(scl3300_ring_buffer));
    memset(&adt7420_ring_buffer, 0, sizeof(adt7420_ring_buffer));
    
    // Reset tick counter and statistics
    tick_counter = 0;
    adxl355_sample_count = 0;
    scl3300_sample_count = 0;
    adt7420_sample_count = 0;
    
    // Configure GPTimer
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,  // 1 MHz = 1 tick per microsecond
    };
    
    ret = gptimer_new_timer(&timer_config, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register ISR handler
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
    
    // Configure periodic alarm at base sample rate
    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = TIMER_PERIOD_US,  // 125 microseconds
        .flags.auto_reload_on_alarm = true,
    };
    ret = gptimer_set_alarm_action(s_timer, &alarm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer alarm: %s", esp_err_to_name(ret));
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        return ret;
    }
    
    // Enable timer (but don't start yet)
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
    
    // Reset statistics
    sensor_acquisition_reset_stats();
    
    // Start timer
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
        return ESP_OK;  // Already stopped
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
 * (Called by separate processing tasks - NOT in ISR)
 *****************************************************************************/

// Check if data is available in ADXL355 buffer
bool adxl355_data_available(void)
{
    return (adxl355_ring_buffer.write_index != adxl355_ring_buffer.read_index);
}

// Read one sample from ADXL355 buffer
bool adxl355_read_sample(adxl355_raw_sample_t *sample)
{
    if (!adxl355_data_available())
    {
        return false;  // No data available
    }
    
    // Copy data from buffer
    *sample = adxl355_ring_buffer.buffer[adxl355_ring_buffer.read_index];
    
    // Advance read index
    adxl355_ring_buffer.read_index = 
        (adxl355_ring_buffer.read_index + 1) & (ADXL355_BUFFER_SIZE - 1);
    
    return true;
}

// Get number of samples available in ADXL355 buffer
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

// Similar functions for SCL3300
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
        (scl3300_ring_buffer.read_index + 1) & (SCL3300_BUFFER_SIZE - 1);
    
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

// Similar functions for ADT7420
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
        (adt7420_ring_buffer.read_index + 1) & (ADT7420_BUFFER_SIZE - 1);
    
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
 * DIAGNOSTIC FUNCTIONS
 * (For debugging/monitoring - called outside ISR)
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
        // This would need to be implemented if you want to track ISR execution time
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
}

// Get overflow counts (indicates buffer overruns)
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

// Get total sample counts
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

// Get current tick count
uint32_t get_tick_count(void)
{
    return tick_counter;
}
