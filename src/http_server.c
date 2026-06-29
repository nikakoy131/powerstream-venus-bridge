#include "http_server.h"
#include "ble_scan.h"
#include "index_html.h"
#include "esp_log.h"
#include "esp_http_server.h"

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

void http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    httpd_uri_t api = {
        .uri = "/api/devices", .method = HTTP_GET, .handler = devices_handler,
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &api);
    ESP_LOGI(TAG, "HTTP server started on port 80");
}
