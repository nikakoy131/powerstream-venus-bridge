#pragma once
#include "esp_err.h"

/* Runtime configuration, persisted in NVS and editable from the web UI.
   On first boot (empty NVS) values are seeded from the optional gitignored
   headers wifi_secrets.h / ps_config.h, else left blank. */
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char user_id[32];   /* EcoFlow account id for BLE auth (optional) */
} settings_t;

void       settings_init(void);          /* load NVS + seed defaults */
settings_t settings_get(void);           /* copy of the current settings */
esp_err_t  settings_save(const settings_t *s);  /* write NVS + update cache */
