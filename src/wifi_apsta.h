#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WIFI_HOSTNAME "powerstream-bridge"

typedef struct {
    bool sta_enabled;      /* STA SSID configured */
    bool sta_connected;    /* associated + got IP */
    char sta_ip[16];       /* "0.0.0.0" until connected */
    char sta_ssid[33];
    int8_t sta_rssi;       /* valid when connected */
    int  retry_count;      /* reconnect attempts since last success */
    int  ap_clients;       /* stations joined to our AP */
} wifi_status_t;

void wifi_apsta_start(void);
void wifi_apsta_get_status(wifi_status_t *out);
