#include "http_server.h"
#include "ble_scan.h"
#include "ps_data.h"
#include "settings.h"
#include "wifi_apsta.h"
#include "weblog.h"
#include "index_html.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

#include <stdlib.h>
#include <string.h>
#include "lwip/sockets.h"

#define TAG "http"
#define MAX_DEVICES 40

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, INDEX_HTML);
    return ESP_OK;
}

static esp_err_t devices_handler(httpd_req_t *req)
{
    static char buf[MAX_DEVICES * 128 + 8];
    ble_scan_results_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t powerstream_handler(httpd_req_t *req)
{
    static char buf[768];
    ps_data_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ---- settings ---- */

static int json_esc(char *dst, int dlen, const char *src)
{
    int n = 0;
    for (; *src && n < dlen - 2; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
        else if (c >= 0x20)        { dst[n++] = c; }
    }
    dst[n] = '\0';
    return n;
}

static void url_decode(char *out, int outlen, const char *src, size_t srclen)
{
    int o = 0;
    for (size_t i = 0; i < srclen && o < outlen - 1; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < srclen) {
            char h[3] = { src[i + 1], src[i + 2], 0 };
            c = (char)strtol(h, NULL, 16);
            i += 2;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

/* Extract a form field (application/x-www-form-urlencoded) into out. */
static bool form_get(const char *body, const char *key, char *out, int outlen)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            url_decode(out, outlen, v, e ? (size_t)(e - v) : strlen(v));
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_status_t w;
    wifi_apsta_get_status(&w);
    char ssid[80], apssid[80], buf[512];
    json_esc(ssid, sizeof(ssid), w.sta_ssid);
    json_esc(apssid, sizeof(apssid), w.ap_ssid);
    snprintf(buf, sizeof(buf),
             "{\"hostname\":\"" WIFI_HOSTNAME "\","
             "\"uptime_s\":%lld,"
             "\"heap_free\":%lu,"
             "\"sta\":{\"enabled\":%s,\"connected\":%s,\"ssid\":\"%s\","
             "\"ip\":\"%s\",\"rssi\":%d,\"retries\":%d},"
             "\"ap\":{\"ssid\":\"%s\",\"ip\":\"192.168.4.1\","
             "\"active\":%s,\"fallback\":%s,\"clients\":%d}}",
             (long long)(esp_timer_get_time() / 1000000),
             (unsigned long)esp_get_free_heap_size(),
             w.sta_enabled ? "true" : "false",
             w.sta_connected ? "true" : "false",
             ssid, w.sta_ip, w.sta_rssi, w.retry_count,
             apssid,
             w.ap_active ? "true" : "false",
             w.ap_fallback ? "true" : "false",
             w.ap_clients);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t log_handler(httpd_req_t *req)
{
    static char buf[8192 + 1];
    weblog_read(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    settings_t cfg = settings_get();
    char ssid[80], uid[72], apssid[80], buf[384];
    json_esc(ssid, sizeof(ssid), cfg.wifi_ssid);
    json_esc(uid, sizeof(uid), cfg.user_id);
    json_esc(apssid, sizeof(apssid), cfg.ap_ssid);
    snprintf(buf, sizeof(buf),
             "{\"wifi_ssid\":\"%s\",\"user_id\":\"%s\",\"wifi_pass_set\":%s,"
             "\"ap_ssid\":\"%s\",\"ap_enabled\":%s,\"ap_pass_set\":%s}",
             ssid, uid, cfg.wifi_pass[0] ? "true" : "false",
             apssid, cfg.ap_enabled ? "true" : "false",
             cfg.ap_pass[0] ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static void reboot_cb(void *arg) { (void)arg; esp_restart(); }

static void schedule_reboot(void)
{
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_once(t, 1500 * 1000);
}

/* Flash-over-WiFi: POST the raw app image, e.g.
     curl --data-binary @firmware.bin http://powerstream-bridge.lan/api/ota */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    int total = req->content_len;
    if (!part || total <= 0 || total > (int)part->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"bad image size\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA start: %d bytes -> %s", total, part->label);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"ota_begin\"}");
        return ESP_OK;
    }

    static char buf[4096];   /* httpd task stack is small; keep this off it */
    int recvd = 0;
    while (recvd < total) {
        int want = total - recvd;
        if (want > (int)sizeof(buf)) want = sizeof(buf);
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) {
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "OTA recv failed at %d/%d", recvd, total);
            return ESP_FAIL;
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"flash write\"}");
            return ESP_OK;
        }
        recvd += r;
    }

    err = esp_ota_end(ota);   /* validates image magic + checksum */
    if (err == ESP_OK)
        err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"invalid image\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA OK (%d bytes), rebooting into %s", recvd, part->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len;
    if (total > (int)sizeof(body) - 1) total = sizeof(body) - 1;
    int recvd = 0;
    while (recvd < total) {
        int r = httpd_req_recv(req, body + recvd, total - recvd);
        if (r <= 0) return ESP_FAIL;
        recvd += r;
    }
    body[recvd] = '\0';

    settings_t cfg = settings_get();
    char val[96];
    if (form_get(body, "wifi_ssid", val, sizeof(val)))
        strlcpy(cfg.wifi_ssid, val, sizeof(cfg.wifi_ssid));
    if (form_get(body, "user_id", val, sizeof(val)))
        strlcpy(cfg.user_id, val, sizeof(cfg.user_id));
    /* Only replace the password when a non-empty value is submitted. */
    if (form_get(body, "wifi_pass", val, sizeof(val)) && val[0])
        strlcpy(cfg.wifi_pass, val, sizeof(cfg.wifi_pass));

    /* AP name may not be blank — the fallback AP needs an SSID to appear as. */
    if (form_get(body, "ap_ssid", val, sizeof(val)) && val[0])
        strlcpy(cfg.ap_ssid, val, sizeof(cfg.ap_ssid));
    if (form_get(body, "ap_enabled", val, sizeof(val)))
        cfg.ap_enabled = (val[0] == '1');
    /* Empty AP password = open network, requested via explicit clear flag;
       otherwise blank means "keep current" like wifi_pass. */
    if (form_get(body, "ap_pass_clear", val, sizeof(val)) && val[0] == '1') {
        cfg.ap_pass[0] = '\0';
    } else if (form_get(body, "ap_pass", val, sizeof(val)) && val[0]) {
        if (strlen(val) < 8) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"ok\":false,\"err\":\"AP password needs 8+ characters (WPA2)\"}");
            return ESP_OK;
        }
        strlcpy(cfg.ap_pass, val, sizeof(cfg.ap_pass));
    }

    if (settings_save(&cfg) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    ESP_LOGI(TAG, "settings saved, rebooting");
    schedule_reboot();
    return ESP_OK;
}

/* A client that vanishes without closing (phone leaves WiFi, laptop sleeps)
   never sends a FIN, so its session socket leaks until TCP keepalive probes
   it out. Without this, leaked sessions drain the global lwIP fd pool and
   accept() wedges into ENFILE for every new connection. */
static esp_err_t session_open_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    int yes = 1, idle = 60, intvl = 10, cnt = 3;   /* dead after ~90 s */
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    return ESP_OK;
}

void http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    /* Purge idle connections instead of refusing new ones once the session
       table fills up — stale keep-alive sessions otherwise wedge the
       server into "accept error 23" (ENFILE) until reboot. */
    cfg.lru_purge_enable = true;
    /* lru_purge only fires when the session table is FULL; if the shared
       lwIP fd pool runs dry first (httpd 2 internal + sessions, Modbus
       listener + client), accept() fails with ENFILE while slots look free
       and the purge never runs. Keep the table small enough that it fills —
       and purges — before the pool is exhausted. */
    cfg.max_open_sockets = 5;
    cfg.open_fn = session_open_cb;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t api  = { .uri = "/api/devices", .method = HTTP_GET, .handler = devices_handler };
    httpd_uri_t ps   = { .uri = "/api/powerstream", .method = HTTP_GET, .handler = powerstream_handler };
    httpd_uri_t sget = { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler };
    httpd_uri_t spost= { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler };
    httpd_uri_t stat = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_uri_t wlog = { .uri = "/api/log", .method = HTTP_GET, .handler = log_handler };
    httpd_uri_t ota  = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &api);
    httpd_register_uri_handler(server, &ps);
    httpd_register_uri_handler(server, &sget);
    httpd_register_uri_handler(server, &spost);
    httpd_register_uri_handler(server, &stat);
    httpd_register_uri_handler(server, &wlog);
    httpd_register_uri_handler(server, &ota);
    ESP_LOGI(TAG, "HTTP server started on port 80");
}
