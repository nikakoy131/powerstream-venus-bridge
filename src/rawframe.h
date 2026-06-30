#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* encrypt_type=1 wire framing (RawHeaderAssembler in ha-ef-ble):
   the inner packet's 5-byte header [AA ver lenLE crc8] is sent in clear; the
   remainder is AES-128-CBC encrypted with zero padding to a 16-byte boundary.
   Received frames are the same: plaintext 5-byte header + encrypted body. */

#define RAWFRAME_BUF_LEN 2048

/* Build a frame: header(5) + AES-CBC(zero-pad(inner[5:])). Returns length, 0 on overflow. */
size_t rawframe_build(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t *inner, size_t inner_len,
                      uint8_t *out, size_t cap);

typedef void (*rawframe_cb)(uint8_t *packet, size_t len, void *ctx);

typedef struct {
    uint8_t buf[RAWFRAME_BUF_LEN];
    size_t  buf_len;
    uint8_t key[16];
    uint8_t iv[16];
} rawframe_asm_t;

void rawframe_reset(rawframe_asm_t *a);
void rawframe_set_session(rawframe_asm_t *a, const uint8_t key[16], const uint8_t iv[16]);

/* Append received bytes; emit one callback per complete frame, reconstructed as
   the full plaintext inner packet (header + decrypted body) for packet_parse(). */
void rawframe_feed(rawframe_asm_t *a, const uint8_t *data, size_t len,
                   rawframe_cb cb, void *ctx);
