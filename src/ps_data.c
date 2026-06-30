#include "ps_data.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_mutex;
static ps_state_t        s_state;
static ps_values_t       s_vals;
static uint32_t          s_count;
static uint32_t          s_last_hb_s;
static char              s_name[32];
static char              s_serial[20];
static char              s_mac[18];
static int8_t            s_rssi;

static uint32_t now_s(void)
{
    return xTaskGetTickCount() / configTICK_RATE_HZ;
}

void ps_data_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_state = PS_STATE_SCANNING;
    memset(&s_vals, 0, sizeof(s_vals));
    s_count = 0;
    s_name[0] = '\0';
    s_mac[0] = '\0';
    s_rssi = 0;
}

void ps_data_set_state(ps_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_mutex);
}

void ps_data_set_device(const char *name, const char *serial,
                        const uint8_t mac[6], int8_t rssi)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_name, name ? name : "", sizeof(s_name));
    strlcpy(s_serial, (serial && serial[0]) ? serial : (name ? name : ""), sizeof(s_serial));
    snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_rssi = rssi;
    xSemaphoreGive(s_mutex);
}

void ps_data_update(const ps_values_t *v)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_vals = *v;
    s_count++;
    s_last_hb_s = now_s();
    xSemaphoreGive(s_mutex);
}

bool ps_data_get(ps_values_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_vals;
    bool valid = s_count > 0;
    xSemaphoreGive(s_mutex);
    return valid;
}

void ps_data_get_serial(char *buf, int len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(buf, s_serial, len);
    xSemaphoreGive(s_mutex);
}

static const char *state_str(ps_state_t s)
{
    switch (s) {
    case PS_STATE_SCANNING:       return "scanning";
    case PS_STATE_CONNECTING:     return "connecting";
    case PS_STATE_DISCOVERING:    return "discovering";
    case PS_STATE_KEY_EXCHANGE:   return "key_exchange";
    case PS_STATE_SESSION_KEY:    return "session_key";
    case PS_STATE_AUTHENTICATING: return "authenticating";
    case PS_STATE_STREAMING:      return "streaming";
    case PS_STATE_DISCONNECTED:   return "disconnected";
    default:                      return "unknown";
    }
}

int ps_data_json(char *buf, int len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint32_t age = s_count ? (now_s() - s_last_hb_s) : 0;
    const ps_values_t *v = &s_vals;

    int n = snprintf(buf, len,
        "{\"state\":\"%s\",\"device\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,"
        "\"valid\":%s,\"count\":%lu,\"age\":%lu,"
        "\"pv1\":{\"v\":%ld,\"a\":%ld,\"w\":%ld,\"t\":%ld},"
        "\"pv2\":{\"v\":%ld,\"a\":%ld,\"w\":%ld,\"t\":%ld},"
        "\"bat\":{\"w\":%ld,\"t\":%ld,\"soc\":%lu},"
        "\"inv\":{\"v\":%ld,\"a\":%ld,\"w\":%ld,\"t\":%ld,\"hz\":%ld},"
        "\"load_w\":%lu,\"llc_t\":%ld,"
        "\"supply_priority\":%lu,\"limit_lo\":%lu,\"limit_hi\":%lu}",
        state_str(s_state), s_name, s_mac, (int)s_rssi,
        v->valid ? "true" : "false", (unsigned long)s_count, (unsigned long)age,
        (long)v->pv1_volt, (long)v->pv1_cur, (long)v->pv1_watts, (long)v->pv1_temp,
        (long)v->pv2_volt, (long)v->pv2_cur, (long)v->pv2_watts, (long)v->pv2_temp,
        (long)v->bat_watts, (long)v->bat_temp, (unsigned long)v->bat_soc,
        (long)v->inv_volt, (long)v->inv_cur, (long)v->inv_watts, (long)v->inv_temp,
        (long)v->inv_freq,
        (unsigned long)v->load_watts, (long)v->llc_temp,
        (unsigned long)v->supply_priority,
        (unsigned long)v->limit_lo, (unsigned long)v->limit_hi);

    xSemaphoreGive(s_mutex);
    return n;
}
