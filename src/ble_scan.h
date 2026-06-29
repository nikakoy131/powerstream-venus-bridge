#pragma once

void ble_scan_start(void);

/* Writes a JSON array into buf (max len bytes). Returns bytes written. */
int ble_scan_results_json(char *buf, int len);
