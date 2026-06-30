#include "packet.h"
#include "crypto.h"

#include <string.h>

bool packet_parse(uint8_t *data, size_t len, packet_t *out)
{
    if (len < 5 || data[0] != 0xAA)
        return false;

    uint8_t version_byte = data[1];
    if (version_byte == 4)
        return false; /* V4 not supported here */

    uint8_t version = version_byte & 0x0F;
    bool sentinel = (version_byte & 0x10) != 0;

    if ((version == 2 && len < 18) || (version == 3 && len < 20))
        return false;
    if (version != 2 && version != 3)
        return false;

    uint16_t payload_length = (uint16_t)(data[2] | (data[3] << 8));

    /* Header CRC8 over the first 4 bytes. */
    if (crypto_crc8(data, 4) != data[4])
        return false;

    /* Trailing CRC16 (only for non-sentinel V2/V3). */
    if (!sentinel) {
        if (len < 2)
            return false;
        uint16_t crc = crypto_crc16_arc(data, len - 2);
        uint16_t got = (uint16_t)(data[len - 2] | (data[len - 1] << 8));
        if (crc != got)
            return false;
    }

    const uint8_t *seq = data + 6;
    out->src = data[12];
    out->dst = data[13];

    size_t payload_start;
    if (version == 2) {
        out->cmd_set = data[14];
        out->cmd_id  = data[15];
        payload_start = 16;
    } else {
        out->cmd_set = data[16];
        out->cmd_id  = data[17];
        payload_start = 18;
    }

    if (payload_start + payload_length > len)
        return false;

    uint8_t *payload = data + payload_start;
    size_t plen = payload_length;

    /* Undo the seq[0] XOR mask. */
    if (seq[0] != 0)
        for (size_t i = 0; i < plen; i++)
            payload[i] ^= seq[0];

    /* Strip the 0xBBBB sentinel. */
    if (sentinel && plen >= 2 && payload[plen - 1] == 0xBB && payload[plen - 2] == 0xBB)
        plen -= 2;

    out->payload = payload;
    out->payload_len = plen;
    return true;
}

size_t packet_build_v3(uint8_t *out, size_t cap,
                       uint8_t src, uint8_t dst, uint8_t cmd_set, uint8_t cmd_id,
                       uint8_t dsrc, uint8_t ddst,
                       const uint8_t *payload, size_t payload_len)
{
    size_t total = 18 + payload_len + 2;
    if (total > cap)
        return 0;

    out[0] = 0xAA;
    out[1] = 0x03;
    out[2] = (uint8_t)(payload_len & 0xFF);
    out[3] = (uint8_t)(payload_len >> 8);
    out[4] = crypto_crc8(out, 4);
    out[5] = 0x0D;                 /* product byte (product_id >= 0) */
    memset(out + 6, 0, 4);         /* seq = 0 */
    out[10] = 0x00;
    out[11] = 0x00;
    out[12] = src;
    out[13] = dst;
    out[14] = dsrc;
    out[15] = ddst;
    out[16] = cmd_set;
    out[17] = cmd_id;
    if (payload_len)
        memcpy(out + 18, payload, payload_len);
    uint16_t crc = crypto_crc16_arc(out, 18 + payload_len);
    out[18 + payload_len]     = (uint8_t)(crc & 0xFF);
    out[18 + payload_len + 1] = (uint8_t)(crc >> 8);
    return total;
}
