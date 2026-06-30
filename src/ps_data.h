#pragma once
#include "ps_proto.h"

typedef enum {
    PS_STATE_SCANNING = 0,
    PS_STATE_CONNECTING,
    PS_STATE_DISCOVERING,
    PS_STATE_KEY_EXCHANGE,
    PS_STATE_SESSION_KEY,
    PS_STATE_AUTHENTICATING,
    PS_STATE_STREAMING,
    PS_STATE_DISCONNECTED,
} ps_state_t;

void ps_data_init(void);

/* Connection lifecycle (called from the BLE task). */
void ps_data_set_state(ps_state_t state);
void ps_data_set_device(const char *name, const char *serial,
                        const uint8_t mac[6], int8_t rssi);

/* Store a freshly decoded heartbeat. */
void ps_data_update(const ps_values_t *v);

/* Copy the latest values (for the Modbus/SunSpec layer). Returns true if at
   least one heartbeat has been decoded. */
bool ps_data_get(ps_values_t *out);

/* Copy the device serial (full SN if known, else advertised name). */
void ps_data_get_serial(char *buf, int len);

/* Serialize the current state + values as JSON for the web UI. Returns bytes written. */
int ps_data_json(char *buf, int len);
