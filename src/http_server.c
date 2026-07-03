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

#include <stdlib.h>
#include <string.h>

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
    char ssid[80], buf[384];
    json_esc(ssid, sizeof(ssid), w.sta_ssid);
    snprintf(buf, sizeof(buf),
             "{\"hostname\":\"" WIFI_HOSTNAME "\","
             "\"uptime_s\":%lld,"
             "\"heap_free\":%lu,"
             "\"sta\":{\"enabled\":%s,\"connected\":%s,\"ssid\":\"%s\","
             "\"ip\":\"%s\",\"rssi\":%d,\"retries\":%d},"
             "\"ap\":{\"ssid\":\"PowerStream-Bridge\",\"ip\":\"192.168.4.1\","
             "\"clients\":%d}}",
             esp_timer_get_time() / 1000000,
             (unsigned long)esp_get_free_heap_size(),
             w.sta_enabled ? "true" : "false",
             w.sta_connected ? "true" : "false",
             ssid, w.sta_ip, w.sta_rssi, w.retry_count,
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
    char ssid[80], uid[72], buf[256];
    json_esc(ssid, sizeof(ssid), cfg.wifi_ssid);
    json_esc(uid, sizeof(uid), cfg.user_id);
    snprintf(buf, sizeof(buf),
             "{\"wifi_ssid\":\"%s\",\"user_id\":\"%s\",\"wifi_pass_set\":%s}",
             ssid, uid, cfg.wifi_pass[0] ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static void reboot_cb(void *arg) { (void)arg; esp_restart(); }

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

    if (settings_save(&cfg) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    ESP_LOGI(TAG, "settings saved, rebooting");
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_once(t, 1500 * 1000);
    return ESP_OK;
}

void http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
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
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &api);
    httpd_register_uri_handler(server, &ps);
    httpd_register_uri_handler(server, &sget);
    httpd_register_uri_handler(server, &spost);
    httpd_register_uri_handler(server, &stat);
    httpd_register_uri_handler(server, &wlog);
    ESP_LOGI(TAG, "HTTP server started on port 80");
}
