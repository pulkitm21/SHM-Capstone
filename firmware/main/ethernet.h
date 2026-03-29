#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Ethernet (ESP32-POE-ISO + LAN8720A via ethernet_init helper).
 *        Creates esp_netif, attaches driver, registers events, starts driver.
 */
esp_err_t ethernet_init(void);

/**
 * @brief Set a static IPv4 address on the Ethernet netif (no DHCP/router case).
 *        Also stops DHCP client and marks "got IP" bit internally.
 *
 * Example: ethernet_set_static_ip(192,168,10,20)
 */
esp_err_t ethernet_set_static_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/**
 * @brief Wait until Ethernet got IP event bit is set.
 *        NOTE: In a no-DHCP setup, ethernet_set_static_ip() sets the bit.
 */
esp_err_t ethernet_wait_for_ip(uint32_t timeout_ms);

/**
 * @brief Returns true if ETH_GOT_IP_BIT is set.
 */
bool ethernet_is_connected(void);

/**
 * @brief Get current IP info from netif.
 */
esp_err_t ethernet_get_ip_info(esp_netif_ip_info_t *ip_info);

/**
 * @brief Access the underlying esp_netif.
 *
 * Pass the returned handle to mqtt_mdns_init() to bind mDNS to this interface.
 * This enables broker hostname resolution ("raspberrypi.local") without a
 * hardcoded IP address. Call order must be:
 *
 *   ethernet_init();
 *   ethernet_wait_for_ip(timeout_ms);   // link must be up before mDNS
 *   mqtt_mdns_init(ethernet_get_netif());
 *   mqtt_init();
 */
esp_netif_t *ethernet_get_netif(void);

/**
 * @brief Callback type invoked every time the Ethernet interface obtains an IP.
 *
 * Called from the IP_EVENT_ETH_GOT_IP handler — both at boot and after any
 * reconnect. Use this to start MQTT / SNTP the first time, and let the MQTT
 * client's own reconnect logic handle subsequent broker reconnects.
 *
 * @param netif  The esp_netif that got the IP (same as ethernet_get_netif()).
 */
typedef void (*ethernet_got_ip_cb_t)(esp_netif_t *netif);

/**
 * @brief Register a callback invoked every time an IP address is obtained.
 *
 * Call this before ethernet_init() so the callback is in place before the
 * first IP_EVENT_ETH_GOT_IP fires. Replaces any previously registered callback.
 * Pass NULL to unregister.
 */
void ethernet_set_got_ip_cb(ethernet_got_ip_cb_t cb);

/**
 * @brief Deinitialize Ethernet (stop driver, unregister events, free netif).
 */
esp_err_t ethernet_deinit(void);

#ifdef __cplusplus
}
#endif