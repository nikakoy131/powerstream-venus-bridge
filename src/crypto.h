#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* MD5 one-shot. */
void crypto_md5(const uint8_t *in, size_t len, uint8_t out[16]);

/* AES-128-CBC. `iv` is not modified (a copy is used internally). `len` must be a
   multiple of 16. */
void crypto_aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                            const uint8_t *in, size_t len, uint8_t *out);
void crypto_aes_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                            const uint8_t *in, size_t len, uint8_t *out);

/* CRC-16/ARC (poly 0x8005, reflected, init 0) and CRC-8/CCITT (poly 0x07, init 0). */
uint16_t crypto_crc16_arc(const uint8_t *data, size_t len);
uint8_t  crypto_crc8(const uint8_t *data, size_t len);
