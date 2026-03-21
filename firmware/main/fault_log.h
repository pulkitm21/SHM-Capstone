/**
 * @file fault_log.h
 * @brief Fault logging system for Wind Turbine SHM
 *
 * Fault codes are appended to the outgoing MQTT sensor data packet as a
 * compact integer array under the "f" key. The Raspberry Pi subscriber
 * translates each number to a human-readable string using the table below.
 *
 * JSON example (faults appended to normal data packet):
 *   {"t":123456,"a":[[...]],"i":[...],"T":21.5,"f":[1,7]}
 *
 * If no faults are pending the "f" field is omitted entirely.
 *
 * FAULT CODE TABLE (keep in sync with Raspberry Pi decoder):
 *   1  - Ethernet link down
 *   2  - Ethernet link recovered
 *   3  - Ethernet no IP / timeout
 *   4  - MQTT disconnected
 *   5  - MQTT reconnected
 *   6  - MQTT publish failed
 *   7  - ADXL355 sample dropped
 *   8  - SCL3300 sample dropped
 *   9  - ADT7420 sample dropped
 *   10 - Reboot attempt
 *   11 - Watchdog reset detected at boot
 *   12 - Power loss detected at boot (previous reset was power-on after outage)
 *   13 - Power restored (logged alongside code 12 at boot)
 *   14 - ADXL355 init failed
 *   15 - SCL3300 init failed
 *   16 - ADT7420 init failed
 *   17 - SPI bus error (mid-run)
 *   18 - I2C bus error (mid-run)
 */

#ifndef FAULT_LOG_H
#define FAULT_LOG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * FAULT CODES
 *****************************************************************************/

#define FAULT_ETH_LINK_DOWN         1
#define FAULT_ETH_LINK_RECOVERED    2
#define FAULT_ETH_NO_IP             3
#define FAULT_MQTT_DISCONNECTED     4
#define FAULT_MQTT_RECONNECTED      5
#define FAULT_MQTT_PUBLISH_FAIL     6
#define FAULT_ADXL355_DROPPED       7
#define FAULT_SCL3300_DROPPED       8
#define FAULT_ADT7420_DROPPED       9
#define FAULT_REBOOT_ATTEMPT        10
#define FAULT_WATCHDOG_RESET        11
#define FAULT_POWER_LOSS            12
#define FAULT_POWER_RESTORED        13
#define FAULT_ADXL355_INIT_FAIL     14
#define FAULT_SCL3300_INIT_FAIL     15
#define FAULT_ADT7420_INIT_FAIL     16
#define FAULT_SPI_ERROR             17
#define FAULT_I2C_ERROR             18

/******************************************************************************
 * CONFIGURATION
 *****************************************************************************/

/* Maximum number of fault codes that can be pending at once.
 * If the buffer fills up, older faults are overwritten (newest wins). */
#define FAULT_LOG_MAX_PENDING       16

/******************************************************************************
 * PUBLIC API
 *****************************************************************************/

/**
 * @brief Record a fault code into the pending buffer.
 *
 * Thread-safe. Can be called from any task context.
 * Do NOT call from an ISR — use the processing task to detect and log
 * faults that originate in the ISR (e.g. sample overflows).
 *
 * @param fault_code  One of the FAULT_* defines above.
 */
void fault_log_record(uint8_t fault_code);

/**
 * @brief Returns true if there are unread fault codes waiting to be sent.
 */
bool fault_log_has_pending(void);

/**
 * @brief Append pending fault codes to a JSON buffer as "f":[...] field.
 *
 * Writes the comma-separated "f" key and integer array at the current
 * buffer offset. Clears the pending list after writing.
 *
 * Call this inside mqtt_publish_sensor_data() just before closing the
 * JSON object, only when fault_log_has_pending() returns true.
 *
 * @param buf       Destination character buffer (the JSON being built).
 * @param buf_size  Total size of buf in bytes.
 * @param offset    Current write position in buf.
 * @return          New offset after writing (i.e. offset + bytes written).
 */
int fault_log_append_to_json(char *buf, int buf_size, int offset);

#ifdef __cplusplus
}
#endif

#endif // FAULT_LOG_H
