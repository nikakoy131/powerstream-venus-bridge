#pragma once
#include <stddef.h>
#include <stdint.h>

/* SunSpec register map for Venus OS dbus-fronius auto-detection.
   178 holding registers starting at Modbus address 40000 (0-based index 0):
     [0]   SunS marker 0x53756E53
     [2]   model 1   (Common)    header + 66 data
     [70]  model 101 (Inverter)  header + 50 data  (live)
     [122] model 120 (Nameplate) header + 26 data
     [150] model 123 (Immediate Controls) header + 24 data (writable)
     [176] end marker 0xFFFF / 0
   Model lengths (66/50/26/24) must be exact or detection fails silently.
   Models 120 (WRtg) + 123 (WMaxLimPct) together make dbus-fronius offer
   power limiting ("zero feed-in"); for non-Fronius devices it must be
   enabled once via the inverter's EnableLimiter setting on the GX. */

#define SUNSPEC_NUM_REGS 178
#define SUNSPEC_BASE_ADDR 40000

void sunspec_init(void);

/* Refresh the live (model 101) registers + serial from the latest heartbeat. */
void sunspec_update(void);

/* Copy `count` registers from 0-based `start` into out[] (host byte order).
   Returns the number copied, or 0 if the range is out of bounds. */
int sunspec_read(uint16_t start, uint16_t count, uint16_t *out);

/* Write `count` registers starting at 0-based `start`. Only the WMaxLim
   group of model 123 (Conn..WMaxLim_Ena) is writable; anything else returns
   0 (Modbus illegal-address). A write updates the PowerStream's output
   setpoint over BLE. Returns the number written, or 0. */
int sunspec_write(uint16_t start, uint16_t count, const uint16_t *vals);
