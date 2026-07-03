#include "weblog.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_BUF_SIZE 8192
#define WEBLOG_LINE_MAX 256

static char s_buf[LOG_BUF_SIZE];
static size_t s_head;           /* next write position */
static bool s_wrapped;
static SemaphoreHandle_t s_mutex;
static vprintf_like_t s_orig_vprintf;

static void ring_put(const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_buf[s_head++] = data[i];
        if (s_head == LOG_BUF_SIZE) {
            s_head = 0;
            s_wrapped = true;
        }
    }
}

static int weblog_vprintf(const char *fmt, va_list args)
{
    char line[WEBLOG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        if (n >= (int)sizeof(line))
            n = sizeof(line) - 1;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ring_put(line, n);
            xSemaphoreGive(s_mutex);
        }
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, args) : n;
}

void weblog_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(weblog_vprintf);
}

int weblog_read(char *buf, size_t len)
{
    if (len == 0)
        return 0;
    size_t out = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_wrapped) {
            /* oldest data starts at head; skip the clipped first line */
            size_t skip = 0;
            while (skip < LOG_BUF_SIZE &&
                   s_buf[(s_head + skip) % LOG_BUF_SIZE] != '\n')
                skip++;
            skip++; /* past the newline */
            for (size_t i = skip; i < LOG_BUF_SIZE && out < len - 1; i++)
                buf[out++] = s_buf[(s_head + i) % LOG_BUF_SIZE];
        } else {
            size_t n = s_head;
            if (n > len - 1)
                n = len - 1;
            memcpy(buf, s_buf + (s_head - n), n);
            out = n;
        }
        xSemaphoreGive(s_mutex);
    }
    buf[out] = '\0';
    return (int)out;
}
