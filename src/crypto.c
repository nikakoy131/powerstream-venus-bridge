#include "crypto.h"

#include <string.h>

/* mbedTLS 4.x put the legacy AES API behind private identifiers, so use the
   ESP-IDF hardware AES driver (C6 AES peripheral). MD5 stays on the public
   md.h dispatch interface. */
#include "aes/esp_aes.h"
#include "mbedtls/md.h"

#define AES_ENCRYPT 1
#define AES_DECRYPT 0

/* ---- MD5 ---- */

void crypto_md5(const uint8_t *in, size_t len, uint8_t out[16])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    mbedtls_md(info, in, len, out);
}

/* ---- AES-128-CBC ---- */

void crypto_aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                            const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, key, 128);
    esp_aes_crypt_cbc(&ctx, AES_DECRYPT, len, iv_copy, in, out);
    esp_aes_free(&ctx);
}

void crypto_aes_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                            const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, key, 128);
    esp_aes_crypt_cbc(&ctx, AES_ENCRYPT, len, iv_copy, in, out);
    esp_aes_free(&ctx);
}

/* ---- CRC ---- */

uint16_t crypto_crc16_arc(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

uint8_t crypto_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}
