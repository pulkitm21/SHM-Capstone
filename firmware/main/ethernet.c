/**
 * @file ethernet.c
 * @brief Ethernet Interface Implementation for ESP32-POE-ISO (LAN8720A, RMII)
 *
 * Works for:
 *  - Router/DHCP networks (wait_for_ip uses IP_EVENT_ETH_GOT_IP)
 *  - Direct PC <-> switch <-> ESP32 networks (no DHCP):
 *        call ethernet_set_static_ip(...)
 */

#include "ethernet.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"

#include "driver/gpio.h"

// Needed for IP4_ADDR macro
#include "lwip/inet.h"

// From the ESP-IDF "ethernet_init" helper component (managed component)
#include "ethernet_init.h"

static const char *TAG = "ethernet";

/*******************************************************************************
 * GPIO Configuration for ESP32-POE-ISO
 ******************************************************************************/
#define PHY_RESET_GPIO      GPIO_NUM_12     // PHY reset pin (active low)
#define PHY_RESET_HOLD_MS   300
#define PHY_STABILIZE_MS    50

/*******************************************************************************
 * Event Group for Connection Status
 ******************************************************************************/
#define ETH_CONNECTED_BIT   BIT0
#define ETH_GOT_IP_BIT      BIT1

static EventGroupHandle_t s_eth_event_group = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static bool s_initialized = false;

// ethernet_init_all() allocates an array of handles
static esp_eth_handle_t *s_eth_handles = NULL;
static uint8_t s_eth_port_cnt = 0;

/*******************************************************************************
 * Event Handlers
 ******************************************************************************/
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        if (s_eth_event_group) {
            xEventGroupSetBits(s_eth_event_group, ETH_CONNECTED_BIT);
        }
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet Link Down");
        if (s_eth_event_group) {
            xEventGroupClearBits(s_eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        }
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        if (s_eth_event_group) {
            xEventGroupClearBits(s_eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        }
        break;

    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address (DHCP or netif)");
    ESP_LOGI(TAG, "  IP:      " IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "  Netmask: " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "  Gateway: " IPSTR, IP2STR(&ip_info->gw));

    if (s_eth_event_group) {
        xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    }
}

/*******************************************************************************
 * PHY Reset Sequence
 ******************************************************************************/
static void phy_reset_sequence(void)
{
    ESP_LOGI(TAG, "Performing PHY reset sequence for ESP32-POE-ISO...");

    gpio_reset_pin(PHY_RESET_GPIO);
    gpio_set_direction(PHY_RESET_GPIO, GPIO_MODE_OUTPUT);

    gpio_set_level(PHY_RESET_GPIO, 0);
    ESP_LOGI(TAG, "  PHY reset asserted (GPIO%d low)", PHY_RESET_GPIO);
    vTaskDelay(pdMS_TO_TICKS(PHY_RESET_HOLD_MS));

    gpio_set_level(PHY_RESET_GPIO, 1);
    ESP_LOGI(TAG, "  PHY reset released");
    vTaskDelay(pdMS_TO_TICKS(PHY_STABILIZE_MS));

    ESP_LOGI(TAG, "PHY reset sequence complete");
}

/*******************************************************************************
 * Public API
 ******************************************************************************/
esp_err_t ethernet_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Ethernet already initialized");
        return ESP_OK;
    }

    esp_err_t ret;
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;

    ESP_LOGI(TAG, "Initializing Ethernet...");

    s_eth_event_group = xEventGroupCreate();
    if (s_eth_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    // Create netif + default loop 
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    phy_reset_sequence();

    ret = ethernet_init_all(&eth_handles, &eth_port_cnt);
    if (ret != ESP_OK || eth_port_cnt == 0 || eth_handles == NULL) {
        ESP_LOGE(TAG, "ethernet_init_all failed");
        goto fail;
    }

    s_eth_handles = eth_handles;
    s_eth_port_cnt = eth_port_cnt;
    s_eth_handle = eth_handles[0];

    ESP_LOGI(TAG, "Ethernet driver initialized");

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create netif");
        goto fail;
    }

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ret = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach netif: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH handler: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP handler: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Ethernet: %s", esp_err_to_name(ret));
        goto fail;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Ethernet initialization complete");
    return ESP_OK;

fail:
    if (s_eth_handles && s_eth_port_cnt > 0) {
        for (uint8_t i = 0; i < s_eth_port_cnt; i++) {
            if (s_eth_handles[i]) {
                esp_eth_driver_uninstall(s_eth_handles[i]);
            }
        }
        free(s_eth_handles);
    }
    s_eth_handles = NULL;
    s_eth_port_cnt = 0;

    if (s_eth_glue) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }
    if (s_eth_event_group) {
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
    }

    s_eth_handle = NULL;
    s_initialized = false;
    return ESP_FAIL;
}

esp_err_t ethernet_set_static_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    if (!s_initialized || s_eth_netif == NULL) {
        ESP_LOGE(TAG, "Ethernet not initialized; cannot set static IP");
        return ESP_FAIL;
    }

    // Stop DHCP client (no router/DHCP case)
    esp_err_t ret = esp_netif_dhcpc_stop(s_eth_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "dhcpc_stop failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, a, b, c, d);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    IP4_ADDR(&ip_info.gw, 0, 0, 0, 0); // no gateway for direct switch

    ret = esp_netif_set_ip_info(s_eth_netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // ethernet_wait_for_ip() can succeed even without DHCP
    if (s_eth_event_group) {
        xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    }

    ESP_LOGI(TAG, "Static IP set: " IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t ethernet_wait_for_ip(uint32_t timeout_ms)
{
    if (!s_initialized || s_eth_event_group == NULL) {
        ESP_LOGE(TAG, "Ethernet not initialized");
        return ESP_FAIL;
    }

    TickType_t timeout = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(
        s_eth_event_group,
        ETH_GOT_IP_BIT,
        pdFALSE,
        pdTRUE,
        timeout
    );

    if (bits & ETH_GOT_IP_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Timeout waiting for IP address");
    return ESP_ERR_TIMEOUT;
}

bool ethernet_is_connected(void)
{
    if (!s_initialized || s_eth_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_eth_event_group);
    return (bits & ETH_GOT_IP_BIT) != 0;
}

esp_err_t ethernet_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (!s_initialized || s_eth_netif == NULL || ip_info == NULL) {
        return ESP_FAIL;
    }
    return esp_netif_get_ip_info(s_eth_netif, ip_info);
}

esp_netif_t *ethernet_get_netif(void)
{
    return s_eth_netif;
}

esp_err_t ethernet_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing Ethernet...");

    if (s_eth_handle) {
        esp_eth_stop(s_eth_handle);
    }

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler);
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);

    if (s_eth_glue) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    if (s_eth_handles && s_eth_port_cnt > 0) {
        for (uint8_t i = 0; i < s_eth_port_cnt; i++) {
            if (s_eth_handles[i]) {
                esp_eth_driver_uninstall(s_eth_handles[i]);
            }
        }
        free(s_eth_handles);
    }
    s_eth_handles = NULL;
    s_eth_port_cnt = 0;

    if (s_eth_event_group) {
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
    }

    s_eth_handle = NULL;
    s_initialized = false;

    ESP_LOGI(TAG, "Ethernet deinitialized");
    return ESP_OK;
}
