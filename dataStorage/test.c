/* main.c */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "mqtt_client.h"

static const char *TAG = "ESP32_ETH_MQTT";

/* ---------------- Ethernet Event Handlers ---------------- */
static void eth_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    switch(event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ETH Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "ETH Link Down");
            break;
        default:
            break;
    }
}

static void got_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
}

/* ---------------- MQTT Event Handler ---------------- */
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            // Subscribe to test topic
            esp_mqtt_client_subscribe(event->client, "esp32/test", 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT Msg received: Topic: %.*s | Data: %.*s",
                     event->topic_len, event->topic,
                     event->data_len, event->data);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    mqtt_event_handler_cb(event_data);
}

/* ---------------- Main Application ---------------- */
void app_main(void)
{
    // Initialize NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    
    // Initialize TCP/IP
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default Ethernet interface
    esp_netif_t *eth_netif = esp_netif_new(&esp_netif_default_eth());

    // Configure PHY & MAC
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 0;          // Olimex LAN8720 default
    phy_config.reset_gpio_num = -1;   // Olimex uses internal reset

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan8720(&phy_config);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    // Register Ethernet events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL, NULL));

    // ---------------- MQTT Setup ----------------
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://192.168.20.2:1883"   // Replace with your Pi IP
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    // ---------------- Publish Loop ----------------
    while (1) {
        if (esp_eth_link_get_phy_link_status(eth_handle)) {
            ESP_LOGI(TAG, "Publishing test message...");
            char payload[64];
            snprintf(payload, sizeof(payload), "{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f}", 0.01, 0.02, 9.81);
            esp_mqtt_client_publish(client, "esp32/test", payload, 0, 1, 0);
        } else {
            ESP_LOGW(TAG, "Ethernet not connected, skipping publish");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
