#include "settings.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

/* Optional gitignored dev seeds (empty when absent). */
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#if __has_include("ps_config.h")
#include "ps_config.h"
#endif
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID ""
#endif
#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS ""
#endif
#ifndef PS_USER_ID
#define PS_USER_ID ""
#endif

#define TAG "settings"
#define NS  "psbridge"

static settings_t s_cfg;

static void load_key(nvs_handle_t h, const char *key, char *buf, size_t len,
                     const char *def)
{
    size_t l = len;
    if (nvs_get_str(h, key, buf, &l) != ESP_OK || buf[0] == '\0')
        strlcpy(buf, def, len);
}

void settings_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_key(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), WIFI_STA_SSID);
        load_key(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), WIFI_STA_PASS);
        load_key(h, "user_id",   s_cfg.user_id,   sizeof(s_cfg.user_id),   PS_USER_ID);
        nvs_close(h);
    } else {
        strlcpy(s_cfg.wifi_ssid, WIFI_STA_SSID, sizeof(s_cfg.wifi_ssid));
        strlcpy(s_cfg.wifi_pass, WIFI_STA_PASS, sizeof(s_cfg.wifi_pass));
        strlcpy(s_cfg.user_id,   PS_USER_ID,   sizeof(s_cfg.user_id));
    }
    ESP_LOGI(TAG, "loaded: wifi_ssid='%s' user_id=%s", s_cfg.wifi_ssid,
             s_cfg.user_id[0] ? "set" : "(empty)");
}

settings_t settings_get(void)
{
    return s_cfg;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    nvs_set_str(h, "wifi_ssid", s->wifi_ssid);
    nvs_set_str(h, "wifi_pass", s->wifi_pass);
    nvs_set_str(h, "user_id",   s->user_id);
    e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK)
        s_cfg = *s;
    return e;
}
