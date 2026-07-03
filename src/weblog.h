#pragma once
#include <stddef.h>

/* Capture ESP_LOG output into a RAM ring buffer for the web UI.
   Call before the first subsystem init so early logs are captured. */
void weblog_init(void);

/* Copy the buffered log (oldest first) into buf, NUL-terminated.
   Returns bytes written (excluding NUL). */
int weblog_read(char *buf, size_t len);
