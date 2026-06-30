#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Inner EcoFlow packet (V2/V3). On the wire:
     [AA][version][len:u16LE][crc8(hdr)][product][seq:4][00 00][src][dst]
     (V3 adds:)[dsrc][ddst] [cmd_set][cmd_id][payload][crc16:u16LE]
   Sentinel-format frames (version high nibble 0x10, e.g. 0x13) XOR the payload
   with seq[0] and carry a 0xBBBB sentinel instead of a trailing CRC16. */

typedef struct {
    uint8_t        src;
    uint8_t        dst;
    uint8_t        cmd_set;
    uint8_t        cmd_id;
    const uint8_t *payload;     /* points into the (modified) input buffer */
    size_t         payload_len;
} packet_t;

/* Parse a device packet. Modifies `data` in place to undo the seq[0] XOR mask.
   Returns true on a valid, recognised (V2/V3) packet. */
bool packet_parse(uint8_t *data, size_t len, packet_t *out);

/* Build a V3 packet for sending. Returns total length, or 0 on overflow. */
size_t packet_build_v3(uint8_t *out, size_t cap,
                       uint8_t src, uint8_t dst, uint8_t cmd_set, uint8_t cmd_id,
                       uint8_t dsrc, uint8_t ddst,
                       const uint8_t *payload, size_t payload_len);
