#pragma once
#include <stdint.h>

void ble_scan_start(void);

/* Request the PowerStream's AC output setpoint ("custom load power") in
   deci-watts (0..8000 = 0..800 W). Safe to call from any task; the value is
   sent over BLE by a 1 s timer once the link is streaming, deduplicated
   against the last sent value and re-sent after a reconnect. */
void ble_ps_request_watts(int32_t deciwatts);

/* Writes a JSON array into buf (max len bytes). Returns bytes written. */
int ble_scan_results_json(char *buf, int len);
