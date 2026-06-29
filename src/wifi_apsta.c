#include "wifi_apsta.h"
#include "wifi_secrets.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#define AP_SSID     "PowerStream-Bridge"
#define AP_PASS     "powerstream"
#define AP_CHANNEL  1
#define AP_MAX_STA  4

static const char *TAG = "wifi";
static int s_retry = 0;
#define MAX_RETRY 5

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGI(TAG, "STA reconnecting (%d/%d)", s_retry, MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started — SSID: %s  IP: 192.168.4.1", AP_SSID);
    }
}

void wifi_apsta_start(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    bool sta_enabled = (strlen(WIFI_STA_SSID) > 0);
    if (sta_enabled)
        esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, event_handler, NULL);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = sizeof(AP_SSID) - 1,
            .password       = AP_PASS,
            .channel        = AP_CHANNEL,
            .max_connection = AP_MAX_STA,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    if (sta_enabled) {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid,     WIFI_STA_SSID, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password,  WIFI_STA_PASS, sizeof(sta_cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        ESP_LOGI(TAG, "STA: connecting to '%s'", WIFI_STA_SSID);
    }

    esp_wifi_start();
    if (sta_enabled)
        esp_wifi_connect();
}
