#include "ps_proto.h"
#include <string.h>

/* Read a base-128 varint. Returns false if it runs past the end. */
static bool read_varint(const uint8_t *d, size_t len, size_t *i, uint64_t *out)
{
    uint64_t r = 0;
    int shift = 0;
    while (*i < len && shift < 64) {
        uint8_t b = d[(*i)++];
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = r;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool ps_proto_decode(const uint8_t *data, size_t len, ps_values_t *out)
{
    memset(out, 0, sizeof(*out));

    size_t i = 0;
    while (i < len) {
        uint64_t tag;
        if (!read_varint(data, len, &i, &tag))
            return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wire  = (uint32_t)(tag & 7);

        uint64_t v = 0;
        switch (wire) {
        case 0: /* varint */
            if (!read_varint(data, len, &i, &v))
                return false;
            break;
        case 1: /* 64-bit */
            if (i + 8 > len) return false;
            i += 8;
            break;
        case 2: /* length-delimited */ {
            uint64_t l;
            if (!read_varint(data, len, &i, &l) || i + l > len) return false;
            i += l;
            break;
        }
        case 5: /* 32-bit */
            if (i + 4 > len) return false;
            i += 4;
            break;
        default:
            return false; /* groups / unknown wire type */
        }

        if (wire != 0)
            continue;

        int32_t  s = (int32_t)(uint32_t)v; /* int32 fields: low 32 bits, signed */
        uint32_t u = (uint32_t)v;

        switch (field) {
        case 16: out->pv1_volt  = s; break;
        case 18: out->pv1_cur   = s; break;
        case 19: out->pv1_watts = s; break;
        case 20: out->pv1_temp  = s; break;
        case 21: out->pv2_volt  = s; break;
        case 23: out->pv2_cur   = s; break;
        case 24: out->pv2_watts = s; break;
        case 25: out->pv2_temp  = s; break;
        case 29: out->bat_watts = s; break;
        case 30: out->bat_temp  = s; break;
        case 31: out->bat_soc   = u; break;
        case 34: out->llc_temp  = s; break;
        case 36: out->inv_volt  = s; break;
        case 37: out->inv_cur   = s; break;
        case 38: out->inv_watts = s; break;
        case 39: out->inv_temp  = s; break;
        case 40: out->inv_freq  = s; break;
        case 48: out->load_watts = u; break;
        case 50: out->supply_priority = u; break;
        case 51: out->limit_lo  = u; break;
        case 52: out->limit_hi  = u; break;
        default: break;
        }
    }

    out->valid = true;
    return true;
}
