#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Decoded subset of the PowerStream inverter_heartbeat protobuf message.
   Values are raw (as transmitted); scaling is applied by the consumer.
   Units: volts ×10, pv current ×10, inverter current ×1000, watts ×10,
   temps ×10, frequency ×10. A field absent from the message stays 0. */
typedef struct {
    bool     valid;

    int32_t  pv1_volt, pv1_cur, pv1_watts, pv1_temp;
    int32_t  pv2_volt, pv2_cur, pv2_watts, pv2_temp;
    int32_t  bat_watts, bat_temp;
    uint32_t bat_soc;
    int32_t  inv_volt, inv_cur, inv_watts, inv_temp, inv_freq;
    int32_t  llc_temp;
    uint32_t load_watts;
    uint32_t supply_priority, limit_lo, limit_hi;
} ps_values_t;

/* Decode an inverter_heartbeat protobuf payload. Returns true if the buffer
   parsed as well-formed protobuf (recognised fields filled into `out`). */
bool ps_proto_decode(const uint8_t *data, size_t len, ps_values_t *out);
