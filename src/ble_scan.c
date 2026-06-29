#include "ble_scan.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#define MAX_DEVICES 40
#define TAG "ble"

typedef struct {
    uint8_t  mac[6];
    char     name[32];
    int8_t   rssi;
    uint32_t last_seen_s;
    bool     in_use;
} device_t;

static device_t s_devs[MAX_DEVICES];
static SemaphoreHandle_t s_mutex;

static void upsert(const uint8_t *mac, const char *name, int8_t rssi)
{
    uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int empty = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devs[i].in_use && memcmp(s_devs[i].mac, mac, 6) == 0) {
            s_devs[i].rssi = rssi;
            s_devs[i].last_seen_s = now;
            if (name && name[0] && !s_devs[i].name[0])
                strlcpy(s_devs[i].name, name, sizeof(s_devs[i].name));
            xSemaphoreGive(s_mutex);
            return;
        }
        if (!s_devs[i].in_use && empty < 0)
            empty = i;
    }
    if (empty >= 0) {
        s_devs[empty].in_use = true;
        memcpy(s_devs[empty].mac, mac, 6);
        s_devs[empty].rssi = rssi;
        s_devs[empty].last_seen_s = now;
        strlcpy(s_devs[empty].name, name ? name : "", sizeof(s_devs[empty].name));
    }
    xSemaphoreGive(s_mutex);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    if (ev->type != BLE_GAP_EVENT_DISC)
        return 0;

    struct ble_hs_adv_fields fields = {0};
    ble_hs_adv_parse_fields(&fields, ev->disc.data, ev->disc.length_data);

    char name[32] = {0};
    if (fields.name && fields.name_len > 0) {
        size_t n = fields.name_len < (sizeof(name) - 1) ? fields.name_len : (sizeof(name) - 1);
        memcpy(name, fields.name, n);
    }

    upsert(ev->disc.addr.val, name, ev->disc.rssi);
    return 0;
}

static void on_sync(void)
{
    /* Units of 0.625 ms. window < itvl => duty-cycled scan that leaves
       radio airtime for WiFi (shared 2.4 GHz radio on the C6).
       itvl=160 (100 ms), window=48 (30 ms) ~= 30% duty cycle.
       Passive scan: don't transmit scan requests, lighter on coexistence. */
    struct ble_gap_disc_params params = {
        .itvl              = 160,
        .window            = 48,
        .filter_policy     = 0,
        .limited           = 0,
        .passive           = 0,  /* active: send SCAN_REQ to get scan responses with names */
        .filter_duplicates = 0,
    };
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event, NULL);
    ESP_LOGI(TAG, "BLE scan started");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_scan_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(s_devs, 0, sizeof(s_devs));

    nimble_port_init();
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(ble_host_task);
}

/* Escape a string for JSON — only control chars and quotes matter for BLE names */
static int json_escape(char *dst, int dlen, const char *src)
{
    int n = 0;
    for (; *src && n < dlen - 1; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (n + 2 >= dlen) break;
            dst[n++] = '\\';
            dst[n++] = c;
        } else if (c < 0x20) {
            /* skip control chars */
        } else {
            dst[n++] = c;
        }
    }
    dst[n] = '\0';
    return n;
}

int ble_scan_results_json(char *buf, int len)
{
    uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;
    int n = 0;
    bool first = true;

    n += snprintf(buf + n, len - n, "[");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES && n < len - 2; i++) {
        if (!s_devs[i].in_use)
            continue;

        char esc[72];
        json_escape(esc, sizeof(esc), s_devs[i].name);

        n += snprintf(buf + n, len - n,
                      "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                      "\"name\":\"%s\",\"rssi\":%d,\"age\":%lu}",
                      first ? "" : ",",
                      s_devs[i].mac[0], s_devs[i].mac[1], s_devs[i].mac[2],
                      s_devs[i].mac[3], s_devs[i].mac[4], s_devs[i].mac[5],
                      esc,
                      (int)s_devs[i].rssi,
                      (unsigned long)(now - s_devs[i].last_seen_s));
        first = false;
    }
    xSemaphoreGive(s_mutex);

    n += snprintf(buf + n, len - n, "]");
    return n;
}
