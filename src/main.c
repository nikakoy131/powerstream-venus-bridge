#include "esp_log.h"
#include "nvs_flash.h"
#include "weblog.h"
#include "settings.h"
#include "wifi_apsta.h"
#include "ble_scan.h"
#include "ps_data.h"
#include "http_server.h"
#include "modbus.h"

#define TAG "main"

void app_main(void)
{
    weblog_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    settings_init();
    ps_data_init();
    wifi_apsta_start();
    ble_scan_start();
    http_server_start();
    modbus_start();

    ESP_LOGI(TAG, "Ready — AP: 192.168.4.1  web UI: http://192.168.4.1");
}
