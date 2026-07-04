#include "wifi_apsta.h"
#include "settings.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#define AP_SSID     "PowerStream-Bridge"
#define AP_PASS     "powerstream"
#define AP_CHANNEL  1
#define AP_MAX_STA  4

/* Retry immediately a few times, then keep trying forever every RETRY_SLOW_MS.
   The old firmware gave up after 5 attempts and stayed offline until reboot. */
#define RETRY_FAST      5
#define RETRY_SLOW_MS   10000

static const char *TAG = "wifi";
static int s_retry = 0;
static bool s_got_ip = false;
static char s_ip[16] = "0.0.0.0";
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "STA reconnecting (attempt %d)", s_retry);
    esp_wifi_connect();
}

static esp_netif_t *s_sta_netif;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* The netif must be started before the hostname can be set (calling
           earlier fails with IF_NOT_READY and the DHCP client then sends
           lwIP's default "espressif"). DHCP starts on association, so setting
           it here is early enough. */
        esp_err_t e = esp_netif_set_hostname(s_sta_netif, WIFI_HOSTNAME);
        if (e != ESP_OK)
            ESP_LOGW(TAG, "set_hostname failed: %s", esp_err_to_name(e));
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        s_retry++;
        if (s_retry <= RETRY_FAST) {
            ESP_LOGI(TAG, "STA reconnecting (%d)", s_retry);
            esp_wifi_connect();
        } else if (!esp_timer_is_active(s_retry_timer)) {
            ESP_LOGW(TAG, "STA down, retrying every %d s", RETRY_SLOW_MS / 1000);
            esp_timer_start_periodic(s_retry_timer, (uint64_t)RETRY_SLOW_MS * 1000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s (hostname: %s)", s_ip, WIFI_HOSTNAME);
        esp_timer_stop(s_retry_timer);
        s_retry = 0;
        s_got_ip = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started — SSID: %s  IP: 192.168.4.1", AP_SSID);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client connected to AP");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "STA disconnected from AP");
    }
}

void wifi_apsta_start(void)
{
    settings_t scfg = settings_get();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    bool sta_enabled = (strlen(scfg.wifi_ssid) > 0);
    if (sta_enabled)
        s_sta_netif = esp_netif_create_default_wifi_sta();

    const esp_timer_create_args_t targs = {
        .callback = retry_cb, .name = "wifi_retry",
    };
    esp_timer_create(&targs, &s_retry_timer);

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
            .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP,
            .pmf_cfg        = { .capable = false, .required = false },
        },
    };

    esp_wifi_set_mode(sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    if (sta_enabled) {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid,     scfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, scfg.wifi_pass, sizeof(sta_cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        ESP_LOGI(TAG, "STA: connecting to '%s'", scfg.wifi_ssid);
    }

    esp_wifi_start();

    /* BLE coexistence forces modem power-save, which makes the STA sleep
       through broadcast ARP requests — the device then looks unreachable to
       any host that doesn't already have its MAC cached. Trade power for
       reachability. */
    esp_err_t pe = esp_wifi_set_ps(WIFI_PS_NONE);
    if (pe != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(pe));

    /* esp_wifi_connect() happens in the WIFI_EVENT_STA_START handler, after
       the hostname is set. */
}

void wifi_apsta_get_status(wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    settings_t scfg = settings_get();
    out->sta_enabled = (strlen(scfg.wifi_ssid) > 0);
    strlcpy(out->sta_ssid, scfg.wifi_ssid, sizeof(out->sta_ssid));
    out->sta_connected = s_got_ip;
    strlcpy(out->sta_ip, s_ip, sizeof(out->sta_ip));
    out->retry_count = s_retry;

    if (s_got_ip) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            out->sta_rssi = ap.rssi;
    }
    wifi_sta_list_t stas;
    if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK)
        out->ap_clients = stas.num;
}
