#include "modbus.h"
#include "sunspec.h"

#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define TAG "modbus"
#define MODBUS_PORT 502

/* Read exactly n bytes; returns false on EOF/error. */
static bool recv_all(int sock, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = recv(sock, buf + got, n - got, 0);
        if (r <= 0)
            return false;
        got += r;
    }
    return true;
}

/* Build an exception response into `resp` (reusing the request's MBAP). */
static int make_exception(uint8_t *resp, const uint8_t *mbap, uint8_t fc, uint8_t code)
{
    memcpy(resp, mbap, 7);          /* txn, proto, len, unit */
    resp[4] = 0; resp[5] = 3;       /* length = unit + fc + code */
    resp[7] = fc | 0x80;
    resp[8] = code;
    return 9;
}

static void handle_client(int sock)
{
    uint8_t mbap[7];
    while (recv_all(sock, mbap, 7)) {
        uint16_t len = (mbap[4] << 8) | mbap[5];   /* unit id + PDU */
        if (len < 2 || len > 255)
            break;
        uint8_t pdu[255];
        if (!recv_all(sock, pdu, len - 1))         /* unit id already in mbap[6] */
            break;

        uint8_t fc = pdu[0];
        uint8_t resp[7 + 2 + 2 * SUNSPEC_NUM_REGS];
        int resp_len;

        if (fc == 0x03) {                          /* Read Holding Registers */
            uint16_t addr = (pdu[1] << 8) | pdu[2];
            uint16_t qty  = (pdu[3] << 8) | pdu[4];
            uint16_t base = (addr >= SUNSPEC_BASE_ADDR) ? addr - SUNSPEC_BASE_ADDR : addr;

            uint16_t regs[SUNSPEC_NUM_REGS];
            if (qty == 0 || qty > SUNSPEC_NUM_REGS) {
                resp_len = make_exception(resp, mbap, fc, 0x03);  /* illegal data value */
            } else {
                sunspec_update();
                if (sunspec_read(base, qty, regs) != qty) {
                    resp_len = make_exception(resp, mbap, fc, 0x02); /* illegal address */
                } else {
                    memcpy(resp, mbap, 4);
                    uint16_t plen = 1 + 1 + 1 + 2 * qty;   /* unit + fc + bytecount + data */
                    resp[4] = (uint8_t)(plen >> 8);
                    resp[5] = (uint8_t)(plen & 0xFF);
                    resp[6] = mbap[6];                     /* unit id */
                    resp[7] = 0x03;
                    resp[8] = (uint8_t)(2 * qty);
                    for (uint16_t i = 0; i < qty; i++) {
                        resp[9 + 2 * i]     = (uint8_t)(regs[i] >> 8);
                        resp[9 + 2 * i + 1] = (uint8_t)(regs[i] & 0xFF);
                    }
                    resp_len = 9 + 2 * qty;
                }
            }
        } else {
            resp_len = make_exception(resp, mbap, fc, 0x01);  /* illegal function */
        }

        if (send(sock, resp, resp_len, 0) < 0)
            break;
    }
}

static void modbus_task(void *arg)
{
    (void)arg;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(MODBUS_PORT),
    };
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(:502) failed: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    listen(listen_sock, 2);
    ESP_LOGI(TAG, "Modbus-TCP server listening on :%d", MODBUS_PORT);

    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int sock = accept(listen_sock, (struct sockaddr *)&src, &slen);
        if (sock < 0)
            continue;
        int ka = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &ka, sizeof(ka));
        ESP_LOGI(TAG, "client connected");
        handle_client(sock);
        close(sock);
        ESP_LOGI(TAG, "client disconnected");
    }
}

void modbus_start(void)
{
    sunspec_init();
    xTaskCreate(modbus_task, "modbus", 4096, NULL, 5, NULL);
}
