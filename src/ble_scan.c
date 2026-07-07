#include "ble_scan.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "crypto.h"
#include "rawframe.h"
#include "packet.h"
#include "ps_proto.h"
#include "ps_data.h"
#include "settings.h"

#define MAX_DEVICES 40
#define TAG "ble"

/* Heartbeats arrive ~once/sec; a long connection interval leaves the shared
   2.4 GHz radio free for WiFi coexistence far more often than NimBLE's
   aggressive ~30-50ms default, which was starving WiFi (STA associated but
   went 30+s without RX/TX). Applied both to the initial connect request and
   enforced against the peer's own BLE_GAP_EVENT_CONN_UPDATE_REQ, since the
   PowerStream renegotiates back to a short interval if we don't override it. */
#define PS_CONN_ITVL       320  /* 400 ms, in 1.25 ms units */
#define PS_CONN_LATENCY    0
#define PS_CONN_SUPERVISION 600 /* 6 s, in 10 ms units */

/* --- our role in the EcoFlow packet header --- */
#define PKT_SRC      0x21
#define AUTH_DST     0x35
#define CMD_SET_SYS  0x35
#define CMD_ID_AUTH_STATUS 0x89   /* keepalive / heartbeat request */
#define CMD_ID_AUTH        0x86   /* auth packet                   */

/* PowerStream inverter_heartbeat: src=0x35 cmd_set=0x14 cmd_id=0x01 */
#define HB_SRC      0x35
#define HB_CMD_SET  0x14
#define HB_CMD_ID   0x01

/* Set "custom load power": cmd_set=0x14 cmd_id=0x81, payload is a
   permanent_watts_pack protobuf { permanent_watts(1): varint deci-watts }.
   The device echoes the value back as inverter_heartbeat field 48
   (load_watts), so the web UI / heartbeat log confirm each change. */
#define PS_CMD_SET      0x14
#define CMD_ID_SET_WATTS 0x81

/* ---- GATT characteristic UUIDs (two possible transports) ---- */
/* Nordic UART: write 6e400002-…, notify 6e400003-… (bytes little-endian) */
static const ble_uuid128_t UART_WRITE = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
static const ble_uuid128_t UART_NOTIFY = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);
/* RFCOMM-like: 00000002/00000003 in the Bluetooth base => 16-bit 0x0002/0x0003 */
static const ble_uuid16_t RFCOMM_WRITE  = BLE_UUID16_INIT(0x0002);
static const ble_uuid16_t RFCOMM_NOTIFY = BLE_UUID16_INIT(0x0003);

/* ===================== scan table (web UI) ===================== */

typedef struct {
    uint8_t  mac[6];
    char     name[32];
    char     serial[20];   /* full SN from the 0xB5B5 advert (for BLE auth) */
    int8_t   rssi;
    uint32_t last_seen_s;
    bool     in_use;
} device_t;

static device_t s_devs[MAX_DEVICES];
static SemaphoreHandle_t s_mutex;

/* Store the full serial parsed from a device's 0xB5B5 advert, keyed by MAC. */
static void store_serial(const uint8_t *mac, const char *serial)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devs[i].in_use && memcmp(s_devs[i].mac, mac, 6) == 0) {
            if (!s_devs[i].serial[0]) {
                strlcpy(s_devs[i].serial, serial, sizeof(s_devs[i].serial));
                ESP_LOGI(TAG, "captured serial from advert: %s", serial);
            }
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

/* Look up a stored serial by MAC. Returns true and fills `out` if found. */
static bool lookup_serial(const uint8_t *mac, char *out, size_t out_len)
{
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devs[i].in_use && memcmp(s_devs[i].mac, mac, 6) == 0 && s_devs[i].serial[0]) {
            strlcpy(out, s_devs[i].serial, out_len);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

static void upsert(const uint8_t *mac, const char *name, int8_t rssi)
{
    uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int empty = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devs[i].in_use && memcmp(s_devs[i].mac, mac, 6) == 0) {
            s_devs[i].rssi = rssi;
            s_devs[i].last_seen_s = now;
            if (name && name[0] && !s_devs[i].name[0])
                strlcpy(s_devs[i].name, name, sizeof(s_devs[i].name));
            xSemaphoreGive(s_mutex);
            return;
        }
        if (!s_devs[i].in_use && empty < 0)
            empty = i;
    }
    if (empty >= 0) {
        s_devs[empty].in_use = true;
        memcpy(s_devs[empty].mac, mac, 6);
        s_devs[empty].rssi = rssi;
        s_devs[empty].last_seen_s = now;
        strlcpy(s_devs[empty].name, name ? name : "", sizeof(s_devs[empty].name));
    }
    xSemaphoreGive(s_mutex);
}

/* ===================== connection / handshake ===================== */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool     s_connecting;
static bool     s_disc_started;
static uint16_t s_write_handle, s_notify_handle, s_cccd_handle;
static char     s_dev_sn[32];

static uint8_t  s_iv[16];          /* md5(reversed dev_sn) */
static uint8_t  s_session_key[16]; /* md5(dev_sn)          */

static rawframe_asm_t s_asm;
static ps_state_t     s_phase;

static void start_scan(void);
static void wq_start_next(void);
static void start_discovery(void);

/* ---- serialized GATT write queue (one client procedure at a time) ---- */
#define WQ_SLOTS 8
#define WQ_SLOT_LEN 256
static uint8_t s_wq[WQ_SLOTS][WQ_SLOT_LEN];
static size_t  s_wq_len[WQ_SLOTS];
static int     s_wq_head, s_wq_tail;
static bool    s_wq_busy;

static void wq_reset(void)
{
    s_wq_head = s_wq_tail = 0;
    s_wq_busy = false;
}

static void wq_enqueue(const uint8_t *data, size_t len)
{
    if (len == 0 || len > WQ_SLOT_LEN)
        return;
    int next = (s_wq_tail + 1) % WQ_SLOTS;
    if (next == s_wq_head) {
        ESP_LOGW(TAG, "write queue full, dropping frame");
        return;
    }
    memcpy(s_wq[s_wq_tail], data, len);
    s_wq_len[s_wq_tail] = len;
    s_wq_tail = next;
    if (!s_wq_busy)
        wq_start_next();
}

static int wq_done_cb(uint16_t conn, const struct ble_gatt_error *err,
                      struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0)
        ESP_LOGW(TAG, "write failed: status=%d", err->status);
    s_wq_busy = false;
    wq_start_next();
    return 0;
}

static void wq_start_next(void)
{
    if (s_wq_busy || s_wq_head == s_wq_tail || s_conn == BLE_HS_CONN_HANDLE_NONE)
        return;
    s_wq_busy = true;
    int rc = ble_gattc_write_flat(s_conn, s_write_handle,
                                  s_wq[s_wq_head], s_wq_len[s_wq_head],
                                  wq_done_cb, NULL);
    s_wq_head = (s_wq_head + 1) % WQ_SLOTS;
    if (rc != 0) {
        ESP_LOGW(TAG, "write start failed: rc=%d", rc);
        s_wq_busy = false;
    }
}

/* ---- send helpers ---- */

static void send_session(uint8_t cmd_set, uint8_t cmd_id,
                         const uint8_t *pl, size_t pl_len)
{
    uint8_t inner[160];
    size_t in = packet_build_v3(inner, sizeof(inner), PKT_SRC, AUTH_DST,
                                cmd_set, cmd_id, 0x01, 0x01, pl, pl_len);
    if (!in) return;
    uint8_t frame[256];
    size_t n = rawframe_build(s_session_key, s_iv, inner, in, frame, sizeof(frame));
    if (n) wq_enqueue(frame, n);
}

static void send_auth(void)
{
    /* MD5(user_id + dev_sn).hexdigest().upper() as 32 ASCII bytes. */
    settings_t cfg = settings_get();
    char concat[96];
    int cl = snprintf(concat, sizeof(concat), "%s%s", cfg.user_id, s_dev_sn);
    uint8_t md5[16];
    crypto_md5((const uint8_t *)concat, cl, md5);
    uint8_t payload[32];
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        payload[i * 2]     = hex[md5[i] >> 4];
        payload[i * 2 + 1] = hex[md5[i] & 0xF];
    }
    ESP_LOGI(TAG, "sending auth (user_id=%s, dev_sn=%s)",
             cfg.user_id[0] ? "set" : "(empty)", s_dev_sn);
    send_session(CMD_SET_SYS, CMD_ID_AUTH, payload, sizeof(payload));
}

/* ---- output setpoint (Venus power limiting via SunSpec model 123) ---- */

/* Aligned 32-bit stores are atomic on the C6, so the Modbus task can post a
   request without locking; the 1 s timer below (same esp_timer task as the
   keepalive) does the actual BLE send. -1 = nothing requested/sent. */
static volatile int32_t s_req_dw = -1;   /* requested setpoint, deci-watts */
static int32_t          s_sent_dw = -1;  /* last value sent this connection */

void ble_ps_request_watts(int32_t deciwatts)
{
    if (deciwatts < 0)    deciwatts = 0;
    if (deciwatts > 8000) deciwatts = 8000;
    s_req_dw = deciwatts;
}

static void setpoint_timer_cb(void *arg)
{
    (void)arg;
    int32_t want = s_req_dw;
    if (want < 0 || want == s_sent_dw)
        return;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_phase != PS_STATE_STREAMING)
        return;
    /* permanent_watts_pack: field 1, varint */
    uint8_t pl[8];
    size_t n = 0;
    pl[n++] = 0x08;
    uint32_t v = (uint32_t)want;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        pl[n++] = b | (v ? 0x80 : 0);
    } while (v);
    send_session(PS_CMD_SET, CMD_ID_SET_WATTS, pl, n);
    s_sent_dw = want;
    ESP_LOGI(TAG, "load setpoint -> %ld.%ldW", (long)want / 10, (long)want % 10);
}

static void handle_inner(uint8_t *p, size_t len)
{
    packet_t pkt;
    if (!packet_parse(p, len, &pkt)) {
        ESP_LOGD(TAG, "inner packet parse failed (%d bytes)", (int)len);
        return;
    }
    if (s_phase != PS_STATE_STREAMING) {
        s_phase = PS_STATE_STREAMING;
        ps_data_set_state(PS_STATE_STREAMING);
        ESP_LOGI(TAG, "streaming: first packet src=0x%02x set=0x%02x id=0x%02x",
                 pkt.src, pkt.cmd_set, pkt.cmd_id);
    }

    if (pkt.src == HB_SRC && pkt.cmd_set == HB_CMD_SET && pkt.cmd_id == HB_CMD_ID) {
        ps_values_t v;
        if (ps_proto_decode(pkt.payload, pkt.payload_len, &v)) {
            ps_data_update(&v);
            ESP_LOGI(TAG,
                "hb: pv1=%ld.%ldV/%ldW pv2=%ld.%ldV/%ldW bat=%lu%%/%ldW "
                "inv=%ld.%ldV/%ldW/%ld.%ldHz load=%ldW",
                (long)v.pv1_volt / 10, (long)v.pv1_volt % 10, (long)v.pv1_watts / 10,
                (long)v.pv2_volt / 10, (long)v.pv2_volt % 10, (long)v.pv2_watts / 10,
                (unsigned long)v.bat_soc, (long)v.bat_watts / 10,
                (long)v.inv_volt / 10, (long)v.inv_volt % 10, (long)v.inv_watts / 10,
                (long)v.inv_freq / 10, (long)v.inv_freq % 10,
                (long)v.load_watts / 10);
        }
    } else {
        ESP_LOGD(TAG, "packet src=0x%02x set=0x%02x id=0x%02x len=%d",
                 pkt.src, pkt.cmd_set, pkt.cmd_id, (int)pkt.payload_len);
    }
}

/* rawframe reassembly callback: every frame is a full inner packet. */
static void on_payload(uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;
    handle_inner(payload, len);
}

/* ---- GATT discovery ---- */

/* Called when the CCCD subscribe write completes; starts the handshake. */
static int subscribe_cb(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err && err->status != 0) {
        ESP_LOGE(TAG, "subscribe failed status=%d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    /* encrypt_type=1: session_key = md5(dev_sn), iv = md5(reversed dev_sn). */
    size_t sl = strlen(s_dev_sn);
    char rev[40];
    if (sl >= sizeof(rev)) sl = sizeof(rev) - 1;
    for (size_t i = 0; i < sl; i++)
        rev[i] = s_dev_sn[sl - 1 - i];
    crypto_md5((const uint8_t *)s_dev_sn, sl, s_session_key);
    crypto_md5((const uint8_t *)rev, sl, s_iv);
    rawframe_set_session(&s_asm, s_session_key, s_iv);

    s_phase = PS_STATE_AUTHENTICATING;
    ps_data_set_state(PS_STATE_AUTHENTICATING);
    ESP_LOGI(TAG, "subscribed, type-1 auth (sn=%s)", s_dev_sn);
    send_session(CMD_SET_SYS, CMD_ID_AUTH_STATUS, NULL, 0);
    send_auth();
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    static const ble_uuid16_t cccd_uuid = BLE_UUID16_INIT(0x2902);
    if (err->status == 0 && dsc) {
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u) == 0)
            s_cccd_handle = dsc->handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (s_cccd_handle == 0) {
            ESP_LOGE(TAG, "no CCCD found for notify char");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "subscribing (cccd=0x%04x)", s_cccd_handle);
        uint8_t val[2] = {0x01, 0x00};
        ble_gattc_write_flat(conn, s_cccd_handle, val, sizeof(val), subscribe_cb, NULL);
    }
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (err->status == 0 && chr) {
        char ub[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&chr->uuid.u, ub);
        ESP_LOGI(TAG, "  chr uuid=%s val=0x%04x props=0x%02x", ub,
                 chr->val_handle, chr->properties);
        if (ble_uuid_cmp(&chr->uuid.u, &UART_WRITE.u) == 0 ||
            ble_uuid_cmp(&chr->uuid.u, &RFCOMM_WRITE.u) == 0)
            s_write_handle = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &UART_NOTIFY.u) == 0 ||
                 ble_uuid_cmp(&chr->uuid.u, &RFCOMM_NOTIFY.u) == 0)
            s_notify_handle = chr->val_handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (s_write_handle == 0 || s_notify_handle == 0) {
            ESP_LOGE(TAG, "char discovery incomplete (w=0x%04x n=0x%04x)",
                     s_write_handle, s_notify_handle);
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "chars: write=0x%04x notify=0x%04x", s_write_handle, s_notify_handle);
        s_cccd_handle = 0;
        ble_gattc_disc_all_dscs(conn, s_notify_handle, 0xffff, dsc_cb, NULL);
    }
    return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "MTU=%d (status %d), discovering chars", mtu, err ? err->status : 0);
    s_write_handle = s_notify_handle = 0;
    ble_gattc_disc_all_chrs(conn, 1, 0xffff, chr_cb, NULL);
    return 0;
}

/* Begin GATT discovery (idempotent — runs once per connection). */
static void start_discovery(void)
{
    if (s_disc_started || s_conn == BLE_HS_CONN_HANDLE_NONE)
        return;
    s_disc_started = true;
    ESP_LOGI(TAG, "exchanging MTU");
    ble_gattc_exchange_mtu(s_conn, mtu_cb, NULL);
}

/* ---- connection lifecycle ---- */

static void reset_link(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_connecting = false;
    s_write_handle = s_notify_handle = s_cccd_handle = 0;
    rawframe_reset(&s_asm);
    wq_reset();
    s_sent_dw = -1;   /* re-send any pending setpoint after reconnect */
    s_phase = PS_STATE_DISCONNECTED;
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    start_scan();
}

static bool is_powerstream(const char *name)
{
    return strncmp(name, "HW51", 4) == 0 || strncmp(name, "EF-HW", 5) == 0;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields = {0};
        ble_hs_adv_parse_fields(&fields, ev->disc.data, ev->disc.length_data);
        char name[32] = {0};
        if (fields.name && fields.name_len > 0) {
            size_t n = fields.name_len < sizeof(name) - 1 ? fields.name_len : sizeof(name) - 1;
            memcpy(name, fields.name, n);
        }
        upsert(ev->disc.addr.val, name, ev->disc.rssi);

        /* EcoFlow puts the full serial + capability flags in its 0xB5B5
           (little-endian) manufacturer-data record: [cid:2][proto][sn:16]... */
        if (fields.mfg_data && fields.mfg_data_len >= 19 &&
            fields.mfg_data[0] == 0xB5 && fields.mfg_data[1] == 0xB5) {
            char sn[17];
            memcpy(sn, fields.mfg_data + 3, 16);
            sn[16] = '\0';
            if (strncmp(sn, "HW51", 4) == 0 || strncmp(sn, "EF", 2) == 0)
                store_serial(ev->disc.addr.val, sn);
        }

        if (!s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE && is_powerstream(name)) {
            /* Need the full serial (from the 0xB5B5 advert) for BLE auth;
               wait for it if this DISC event didn't carry it yet. */
            char sn[20] = {0};
            if (!lookup_serial(ev->disc.addr.val, sn, sizeof(sn)))
                return 0;
            strlcpy(s_dev_sn, sn, sizeof(s_dev_sn));
            ESP_LOGI(TAG, "found PowerStream '%s' sn=%s, connecting", name, s_dev_sn);
            ps_data_set_device(name, s_dev_sn, ev->disc.addr.val, ev->disc.rssi);
            ps_data_set_state(PS_STATE_CONNECTING);
            s_connecting = true;
            ble_gap_disc_cancel();
            struct ble_gap_conn_params conn_params = {
                .scan_itvl = 0x0010,
                .scan_window = 0x0010,
                .itvl_min = PS_CONN_ITVL,
                .itvl_max = PS_CONN_ITVL,
                .latency = PS_CONN_LATENCY,
                .supervision_timeout = PS_CONN_SUPERVISION,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &ev->disc.addr, 30000,
                                     &conn_params, gap_event, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "connect failed rc=%d, rescanning", rc);
                s_connecting = false;
                start_scan();
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_connecting = false;
            s_disc_started = false;
            ps_data_set_state(PS_STATE_DISCOVERING);
            ESP_LOGI(TAG, "connected");
            start_discovery();
        } else {
            ESP_LOGW(TAG, "connect status=%d, rescanning", ev->connect.status);
            reset_link();
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason=%d)", ev->disconnect.reason);
        reset_link();
        ps_data_set_state(PS_STATE_DISCONNECTED);
        {
            static esp_timer_handle_t t;
            if (!t) {
                const esp_timer_create_args_t a = { .callback = reconnect_timer_cb,
                                                    .name = "reconnect" };
                esp_timer_create(&a, &t);
            }
            esp_timer_start_once(t, 10 * 1000 * 1000); /* 10 s */
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t l = OS_MBUF_PKTLEN(ev->notify_rx.om);
        static uint8_t tmp[600];   /* static: single NimBLE host-task caller */
        if (l > sizeof(tmp)) l = sizeof(tmp);
        os_mbuf_copydata(ev->notify_rx.om, 0, l, tmp);
        rawframe_feed(&s_asm, tmp, l, on_payload, NULL);
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d; starting discovery",
                 ev->enc_change.status);
        start_discovery();
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* NO_IO / JustWorks: nothing to do. */
        ESP_LOGI(TAG, "passkey action=%d", ev->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
    case BLE_GAP_EVENT_CONN_UPDATE:
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        /* self_params defaults to a copy of the peer's request; overwrite it
           so our long interval sticks instead of being renegotiated away. */
        ev->conn_update_req.self_params->itvl_min = PS_CONN_ITVL;
        ev->conn_update_req.self_params->itvl_max = PS_CONN_ITVL;
        ev->conn_update_req.self_params->latency = PS_CONN_LATENCY;
        ev->conn_update_req.self_params->supervision_timeout = PS_CONN_SUPERVISION;
        return 0;

    default:
        ESP_LOGI(TAG, "GAP event type=%d (unhandled)", ev->type);
        return 0;
    }
}

/* ---- scan ---- */

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl              = 160,  /* 100 ms */
        .window            = 48,   /* 30 ms  */
        .filter_policy     = 0,
        .limited           = 0,
        .passive           = 0,    /* active: get scan-response names */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGW(TAG, "scan start rc=%d", rc);
    else
        ESP_LOGI(TAG, "BLE scan started");
    ps_data_set_state(PS_STATE_SCANNING);
}

static void keepalive_timer_cb(void *arg)
{
    (void)arg;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE &&
        (s_phase == PS_STATE_STREAMING || s_phase == PS_STATE_AUTHENTICATING))
        send_session(CMD_SET_SYS, CMD_ID_AUTH_STATUS, NULL, 0);
}

static void on_sync(void)
{
    ble_att_set_preferred_mtu(247);
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_scan_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(s_devs, 0, sizeof(s_devs));
    reset_link();
    rawframe_reset(&s_asm);

    nimble_port_init();
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* JustWorks pairing (no IO, no bonding/storage) to test whether the device
       gates its app protocol behind an encrypted link. */
    ble_hs_cfg.sm_io_cap  = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 1;

    static esp_timer_handle_t ka;
    const esp_timer_create_args_t a = { .callback = keepalive_timer_cb, .name = "keepalive" };
    esp_timer_create(&a, &ka);
    esp_timer_start_periodic(ka, 30 * 1000 * 1000); /* 30 s */

    static esp_timer_handle_t sp;
    const esp_timer_create_args_t b = { .callback = setpoint_timer_cb, .name = "setpoint" };
    esp_timer_create(&b, &sp);
    esp_timer_start_periodic(sp, 1000 * 1000); /* 1 s */

    nimble_port_freertos_init(ble_host_task);
}

/* ===================== scan results JSON (web UI) ===================== */

static int json_escape(char *dst, int dlen, const char *src)
{
    int n = 0;
    for (; *src && n < dlen - 1; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (n + 2 >= dlen) break;
            dst[n++] = '\\';
            dst[n++] = c;
        } else if (c >= 0x20) {
            dst[n++] = c;
        }
    }
    dst[n] = '\0';
    return n;
}

int ble_scan_results_json(char *buf, int len)
{
    uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;
    int n = 0;
    bool first = true;
    n += snprintf(buf + n, len - n, "[");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES && n < len - 2; i++) {
        if (!s_devs[i].in_use)
            continue;
        char esc[72];
        json_escape(esc, sizeof(esc), s_devs[i].name);
        n += snprintf(buf + n, len - n,
                      "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                      "\"name\":\"%s\",\"rssi\":%d,\"age\":%lu}",
                      first ? "" : ",",
                      s_devs[i].mac[0], s_devs[i].mac[1], s_devs[i].mac[2],
                      s_devs[i].mac[3], s_devs[i].mac[4], s_devs[i].mac[5],
                      esc, (int)s_devs[i].rssi,
                      (unsigned long)(now - s_devs[i].last_seen_s));
        first = false;
    }
    xSemaphoreGive(s_mutex);
    n += snprintf(buf + n, len - n, "]");
    return n;
}
