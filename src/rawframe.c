#include "rawframe.h"
#include "crypto.h"

#include <string.h>

#define SCRATCH_LEN RAWFRAME_BUF_LEN

size_t rawframe_build(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t *inner, size_t inner_len,
                      uint8_t *out, size_t cap)
{
    if (inner_len < 5)
        return 0;
    size_t body = inner_len - 5;
    size_t enc_len = (body + 15) / 16 * 16;
    if (5 + enc_len > cap || enc_len > SCRATCH_LEN)
        return 0;

    /* static: only ever called from the single NimBLE host task. */
    static uint8_t padded[SCRATCH_LEN];
    memcpy(padded, inner + 5, body);
    memset(padded + body, 0, enc_len - body);

    memcpy(out, inner, 5);                 /* plaintext header */
    crypto_aes_cbc_encrypt(key, iv, padded, enc_len, out + 5);
    return 5 + enc_len;
}

void rawframe_reset(rawframe_asm_t *a)
{
    a->buf_len = 0;
}

void rawframe_set_session(rawframe_asm_t *a, const uint8_t key[16], const uint8_t iv[16])
{
    memcpy(a->key, key, 16);
    memcpy(a->iv, iv, 16);
    a->buf_len = 0;
}

static int overhead_for(uint8_t version)
{
    if (version == 4) return 5;
    if (version >= 3) return 15;
    return 13;
}

void rawframe_feed(rawframe_asm_t *a, const uint8_t *data, size_t len,
                   rawframe_cb cb, void *ctx)
{
    if (len > sizeof(a->buf)) {
        data += (len - sizeof(a->buf));
        len = sizeof(a->buf);
    }
    if (a->buf_len + len > sizeof(a->buf))
        a->buf_len = 0;
    memcpy(a->buf + a->buf_len, data, len);
    a->buf_len += len;

    size_t off = 0;
    while (a->buf_len - off >= 5) {
        uint8_t *p = a->buf + off;
        size_t avail = a->buf_len - off;

        /* Find a 0xAA whose 4-byte header CRC8 checks out. */
        size_t start = 0;
        while (start < avail) {
            if (p[start] == 0xAA && start + 5 <= avail &&
                crypto_crc8(p + start, 4) == p[start + 4])
                break;
            if (p[start] == 0xAA && start + 5 > avail)
                break; /* maybe valid once more bytes arrive */
            start++;
        }
        if (start >= avail) { off = a->buf_len; break; }
        off += start;
        p = a->buf + off;
        avail = a->buf_len - off;
        if (avail < 5) break;
        if (crypto_crc8(p, 4) != p[4]) break; /* header incomplete; wait */

        uint16_t payload_len = (uint16_t)(p[2] | (p[3] << 8));
        uint8_t  version = p[1];
        size_t inner_len = overhead_for(version) + payload_len;
        size_t enc_len = (inner_len + 15) / 16 * 16;
        size_t frame_len = 5 + enc_len;
        if (frame_len > avail) break; /* wait for more */
        if (enc_len > SCRATCH_LEN) { off += 1; continue; }

        /* static scratch: single NimBLE host-task caller, avoids 4 KB of stack. */
        static uint8_t dec[SCRATCH_LEN];
        crypto_aes_cbc_decrypt(a->key, a->iv, p + 5, enc_len, dec);

        static uint8_t pkt[SCRATCH_LEN];
        size_t pkt_len = 5 + inner_len;
        if (pkt_len <= sizeof(pkt)) {
            memcpy(pkt, p, 5);
            memcpy(pkt + 5, dec, inner_len);
            cb(pkt, pkt_len, ctx);
        }
        off += frame_len;
    }

    if (off > 0) {
        size_t rem = a->buf_len - off;
        memmove(a->buf, a->buf + off, rem);
        a->buf_len = rem;
    }
}
