idf_component_register(
    SRCS
        "main.c"
        "wifi_apsta.c"
        "ble_scan.c"
        "http_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        bt
        log
)
