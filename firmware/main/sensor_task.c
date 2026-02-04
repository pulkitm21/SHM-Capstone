/**
 * @file sensor_task.c
 * @brief Timing-Critical Sensor Acquisition System Implementation
 *
 * Design principles for minimal jitter:
 * 
 * 1. ACQUISITION TASK (highest priority):
 *    - NO ESP_LOG calls (takes locks, can block on UART)
 *    - NO floating point formatting
 *    - NO memory allocation
 *    - NO blocking operations except waiting for timer
 *    - Reads sensors in fixed order (no bus contention)
 *    - Uses lock-free queue to pass data
 *
 * 2. PROCESSING TASK (low priority):
 *    - All logging happens here
 *    - All formatting happens here
 *    - Can be preempted at any time
 *    - Reads from queue, never blocks acquisition
 *
 * 3. SINGLE TIMER:
 *    - One hardware timer at base rate
 *    - Decimation counters for slower sensors
 *    - No multiple timer coordination needed
 *
 * Reference:
 * - https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gptimer.html
 */

#include "sensor_task.h"
#include "driver/gptimer.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "adt7420.h"
#include "adxl355.h"
#include "scl3300.h"

static const char *TAG = "sensor_task";

/*******************************************************************************
 * Private Variables
 ******************************************************************************/

// Hardware timer handle
static gptimer_handle_t s_timer = NULL;

// Task handles
static TaskHandle_t s_acquisition_task = NULL;
static TaskHandle_t s_processing_task = NULL;

// Queue for passing samples from acquisition to processing
static QueueHandle_t s_sample_queue = NULL;

// Sensor availability flags
static bool s_temp_available = false;

// Decimation counters (count down to 0, then sample)
static volatile uint32_t s_accel_counter = 0;
static volatile uint32_t s_angle_counter = 0;
static volatile uint32_t s_temp_counter = 0;

// Sample counter (monotonic)
static volatile uint32_t s_sample_number = 0;

// Statistics (for monitoring jitter)
static volatile uint32_t s_samples_acquired = 0;
static volatile uint32_t s_samples_dropped = 0;
static volatile uint32_t s_max_acquisition_time_us = 0;


/*******************************************************************************
 * Timer ISR Callback
 *
 * Minimal work in ISR - just notify the acquisition task.
 * IRAM_ATTR ensures this runs from internal RAM (no flash cache delays).
 ******************************************************************************/

static bool IRAM_ATTR timer_alarm_cb(gptimer_handle_t timer,
                                      const gptimer_alarm_event_data_t *edata,
                                      void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    
    // Notify acquisition task
    if (s_acquisition_task != NULL) {
        vTaskNotifyGiveFromISR(s_acquisition_task, &high_task_woken);
    }
    
    return (high_task_woken == pdTRUE);
}


/*******************************************************************************
 * Acquisition Task (TIMING CRITICAL)
 *
 * This task does the actual sensor reading. It must:
 * - Execute as fast as possible
 * - Not call any blocking functions except ulTaskNotifyTake
 * - Not call ESP_LOG (uses locks, can block)
 * - Not do any formatting or string operations
 * - Use fixed memory (no malloc/free)
 ******************************************************************************/

static void acquisition_task(void *pvParameters)
{
    sensor_sample_t sample;
    int64_t start_time, end_time, elapsed;
    BaseType_t queue_result;
    
    // Pre-declare sensor data structures (avoid stack allocation in loop)
    adxl355_accel_t accel_data;
    scl3300_angle_t angle_data;
    float temp_data;
    
    // Initialize decimation counters
    s_accel_counter = ACCEL_DECIMATION;
    s_angle_counter = ANGLE_DECIMATION;
    s_temp_counter = TEMP_DECIMATION;
    
    for (;;) {
        // Wait for hardware timer notification (this is the ONLY blocking call)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Record start time for jitter measurement
        start_time = esp_timer_get_time();
        
        // Initialize sample
        sample.timestamp_us = start_time;
        sample.sample_number = s_sample_number++;
        sample.flags.accel_valid = 0;
        sample.flags.angle_valid = 0;
        sample.flags.temp_valid = 0;
        
        // ===== ACCELEROMETER (highest rate) =====
        s_accel_counter--;
        if (s_accel_counter == 0) {
            s_accel_counter = ACCEL_DECIMATION;
            
            // Read accelerometer (SPI transaction)
            if (adxl355_read_acceleration(&accel_data) == ESP_OK) {
                sample.accel.x = accel_data.x;
                sample.accel.y = accel_data.y;
                sample.accel.z = accel_data.z;
                sample.flags.accel_valid = 1;
            }
        }
        
        // ===== INCLINOMETER (medium rate) =====
        s_angle_counter--;
        if (s_angle_counter == 0) {
            s_angle_counter = ANGLE_DECIMATION;
            
            // Read inclinometer (SPI transaction - same bus, different CS)
            if (scl3300_read_angle(&angle_data) == ESP_OK) {
                sample.angle.x = angle_data.x;
                sample.angle.y = angle_data.y;
                sample.angle.z = angle_data.z;
                sample.flags.angle_valid = 1;
            }
        }
        
        // ===== TEMPERATURE (lowest rate) =====
        if (s_temp_available) {
            s_temp_counter--;
            if (s_temp_counter == 0) {
                s_temp_counter = TEMP_DECIMATION;
                
                // Read temperature (I2C transaction - different bus)
                if (adt7420_read_temperature(&temp_data) == ESP_OK) {
                    sample.temperature = temp_data;
                    sample.flags.temp_valid = 1;
                }
            }
        }
        
        // ===== SEND TO QUEUE (non-blocking) =====
        // Only send if we have at least one valid reading
        if (sample.flags.accel_valid || sample.flags.angle_valid || sample.flags.temp_valid) {
            // Use xQueueSendFromISR semantics even though we're in a task
            // because we want non-blocking behavior
            queue_result = xQueueSend(s_sample_queue, &sample, 0);
            
            if (queue_result == pdTRUE) {
                s_samples_acquired++;
            } else {
                // Queue full - processing can't keep up
                s_samples_dropped++;
            }
        }
        
        // ===== MEASURE ACQUISITION TIME =====
        end_time = esp_timer_get_time();
        elapsed = end_time - start_time;
        
        // Track maximum (indicates worst-case jitter)
        if (elapsed > s_max_acquisition_time_us) {
            s_max_acquisition_time_us = (uint32_t)elapsed;
        }
    }
}


/*******************************************************************************
 * Processing Task (NON-CRITICAL)
 *
 * This task handles all slow operations:
 * - Reading from queue
 * - Formatting data
 * - Logging to serial
 * - (Future: transmitting over network)
 *
 * Can be preempted at any time without affecting acquisition timing.
 ******************************************************************************/

static void processing_task(void *pvParameters)
{
    sensor_sample_t sample;
    
    // Initial log message (safe here, we're in low priority task)
    ESP_LOGI(TAG, "Processing task started on core %d", xPortGetCoreID());
    
    for (;;) {
        // Wait for sample from queue (blocking is OK here)
        if (xQueueReceive(s_sample_queue, &sample, portMAX_DELAY) == pdTRUE) {
            
            // Process accelerometer data
            if (sample.flags.accel_valid) {
                ESP_LOGI("ACCEL", "[%lu] X=%.4f g, Y=%.4f g, Z=%.4f g",
                         (unsigned long)sample.sample_number,
                         sample.accel.x, sample.accel.y, sample.accel.z);
            }
            
            // Process inclinometer data
            if (sample.flags.angle_valid) {
                ESP_LOGI("ANGLE", "[%lu] X=%.2f°, Y=%.2f°, Z=%.2f°",
                         (unsigned long)sample.sample_number,
                         sample.angle.x, sample.angle.y, sample.angle.z);
            }
            
            // Process temperature data
            if (sample.flags.temp_valid) {
                ESP_LOGI("TEMP", "[%lu] %.2f°C",
                         (unsigned long)sample.sample_number,
                         sample.temperature);
            }
        }
    }
}


/*******************************************************************************
 * Timer Initialization
 ******************************************************************************/

static esp_err_t init_timer(void)
{
    esp_err_t ret;
    
    // Timer configuration - 1 MHz resolution
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = GPTIMER_RESOLUTION_HZ,
    };
    
    ret = gptimer_new_timer(&timer_config, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer");
        return ret;
    }
    
    // Register callback (runs in ISR context)
    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_alarm_cb,
    };
    ret = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register timer callback");
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        return ret;
    }
    
    // Configure periodic alarm at base sample rate
    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = BASE_SAMPLE_PERIOD_US,
        .flags.auto_reload_on_alarm = true,
    };
    ret = gptimer_set_alarm_action(s_timer, &alarm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer alarm");
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        return ret;
    }
    
    // Enable timer
    ret = gptimer_enable(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer");
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "Hardware timer initialized (period=%d us, rate=%d Hz)",
             BASE_SAMPLE_PERIOD_US, (int)(1000000 / BASE_SAMPLE_PERIOD_US));
    
    return ESP_OK;
}


/*******************************************************************************
 * Public Functions
 ******************************************************************************/

esp_err_t sensor_acquisition_init(bool temp_sensor_available)
{
    esp_err_t ret;
    BaseType_t task_ret;
    
    s_temp_available = temp_sensor_available;
    
    ESP_LOGI(TAG, "Initializing sensor acquisition system...");
    ESP_LOGI(TAG, "  Base rate: %d Hz", (int)(1000000 / BASE_SAMPLE_PERIOD_US));
    ESP_LOGI(TAG, "  Accel rate: %d Hz (decimation=%d)", 
             (int)(1000000 / BASE_SAMPLE_PERIOD_US / ACCEL_DECIMATION), ACCEL_DECIMATION);
    ESP_LOGI(TAG, "  Angle rate: %d Hz (decimation=%d)",
             (int)(1000000 / BASE_SAMPLE_PERIOD_US / ANGLE_DECIMATION), ANGLE_DECIMATION);
    ESP_LOGI(TAG, "  Temp rate: %d Hz (decimation=%d)",
             (int)(1000000 / BASE_SAMPLE_PERIOD_US / TEMP_DECIMATION), TEMP_DECIMATION);
    
    // Create sample queue
    s_sample_queue = xQueueCreate(SENSOR_QUEUE_SIZE, sizeof(sensor_sample_t));
    if (s_sample_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sample queue");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Sample queue created (size=%d)", SENSOR_QUEUE_SIZE);
    
    // Initialize hardware timer
    ret = init_timer();
    if (ret != ESP_OK) {
        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;
        return ESP_FAIL;
    }
    
    // Create acquisition task (HIGHEST PRIORITY, PINNED TO CORE)
    task_ret = xTaskCreatePinnedToCore(
        acquisition_task,
        "acq_task",
        ACQUISITION_TASK_STACK_SIZE,
        NULL,
        ACQUISITION_TASK_PRIORITY,
        &s_acquisition_task,
        ACQUISITION_TASK_CORE
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create acquisition task");
        gptimer_disable(s_timer);
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Acquisition task created (priority=%d, core=%d)",
             ACQUISITION_TASK_PRIORITY, ACQUISITION_TASK_CORE);
    
    // Create processing task (LOW PRIORITY, CAN FLOAT)
    task_ret = xTaskCreatePinnedToCore(
        processing_task,
        "proc_task",
        PROCESSING_TASK_STACK_SIZE,
        NULL,
        PROCESSING_TASK_PRIORITY,
        &s_processing_task,
        PROCESSING_TASK_CORE
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create processing task");
        // Cleanup
        vTaskDelete(s_acquisition_task);
        s_acquisition_task = NULL;
        gptimer_disable(s_timer);
        gptimer_del_timer(s_timer);
        s_timer = NULL;
        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Processing task created (priority=%d)",
             PROCESSING_TASK_PRIORITY);
    
    ESP_LOGI(TAG, "Sensor acquisition system initialized");
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
        ESP_LOGE(TAG, "Failed to start timer");
        return ESP_FAIL;
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
        ESP_LOGE(TAG, "Failed to stop timer");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Sensor acquisition STOPPED");
    return ESP_OK;
}


void sensor_acquisition_get_stats(uint32_t *samples_acquired,
                                   uint32_t *samples_dropped,
                                   uint32_t *max_acquisition_time_us)
{
    if (samples_acquired) {
        *samples_acquired = s_samples_acquired;
    }
    if (samples_dropped) {
        *samples_dropped = s_samples_dropped;
    }
    if (max_acquisition_time_us) {
        *max_acquisition_time_us = s_max_acquisition_time_us;
    }
}


void sensor_acquisition_reset_stats(void)
{
    s_samples_acquired = 0;
    s_samples_dropped = 0;
    s_max_acquisition_time_us = 0;
    s_sample_number = 0;
}