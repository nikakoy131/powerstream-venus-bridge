#include "sunspec.h"
#include "ps_data.h"

#include <string.h>
#include "esp_timer.h"

/* 0-based register indices of each block within the 152-register map. */
#define M1_HDR   2     /* Common header   */
#define M1_DATA  4     /* Common data (66) */
#define M101_HDR 70    /* Inverter header  */
#define M101     72    /* Inverter data (50) */
#define M120_HDR 122   /* Nameplate header */
#define M120     124   /* Nameplate data (26) */
#define END_HDR  150

#define U16_NA 0xFFFF      /* uint16 / enum "not implemented" */
#define S16_NA 0x8000      /* int16 "not implemented"         */

static uint16_t s_regs[SUNSPEC_NUM_REGS];
static double   s_wh_accum;       /* lifetime Wh accumulator */
static int64_t  s_last_us;

static void set_u16(int idx, uint16_t v) { s_regs[idx] = v; }
static void set_s16(int idx, int16_t v)  { s_regs[idx] = (uint16_t)v; }

static void set_acc32(int idx, uint32_t v)   /* high word first */
{
    s_regs[idx]     = (uint16_t)(v >> 16);
    s_regs[idx + 1] = (uint16_t)(v & 0xFFFF);
}

/* SunSpec string: big-endian, 2 chars per register, NUL-padded. */
static void set_str(int idx, int nregs, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    for (int i = 0; i < nregs; i++) {
        uint8_t hi = (size_t)(2 * i)     < len ? (uint8_t)s[2 * i]     : 0;
        uint8_t lo = (size_t)(2 * i + 1) < len ? (uint8_t)s[2 * i + 1] : 0;
        s_regs[idx + i] = ((uint16_t)hi << 8) | lo;
    }
}

void sunspec_init(void)
{
    memset(s_regs, 0, sizeof(s_regs));

    /* SunS marker */
    set_u16(0, 0x5375);
    set_u16(1, 0x6E53);

    /* Model 1 — Common */
    set_u16(M1_HDR, 1);
    set_u16(M1_HDR + 1, 66);
    set_str(M1_DATA + 0,  16, "EcoFlow");      /* Mn  */
    set_str(M1_DATA + 16, 16, "PowerStream");  /* Md  */
    set_str(M1_DATA + 32, 8,  "");             /* Opt */
    set_str(M1_DATA + 40, 8,  "1.0");          /* Vr  */
    set_str(M1_DATA + 48, 16, "");             /* SN (filled in update) */
    set_u16(M1_DATA + 64, 126);                /* DA  */
    set_u16(M1_DATA + 65, 0);                  /* Pad */

    /* Model 101 — Inverter header (data filled in update) */
    set_u16(M101_HDR, 101);
    set_u16(M101_HDR + 1, 50);

    /* Model 120 — Nameplate */
    set_u16(M120_HDR, 120);
    set_u16(M120_HDR + 1, 26);
    set_u16(M120 + 0, 4);        /* DERTyp = PV       */
    set_u16(M120 + 1, 800);      /* WRtg  = 800 W     */
    set_s16(M120 + 2, 0);        /* WRtg_SF           */
    set_u16(M120 + 3, 800);      /* VARtg             */
    set_s16(M120 + 4, 0);        /* VARtg_SF          */
    for (int i = 5; i <= 8; i++) set_s16(M120 + i, S16_NA);  /* VArRtgQ1..4 */
    set_s16(M120 + 9, 0);        /* VArRtg_SF         */
    set_u16(M120 + 10, U16_NA);  /* ARtg              */
    set_s16(M120 + 11, 0);       /* ARtg_SF           */
    for (int i = 12; i <= 15; i++) set_s16(M120 + i, S16_NA);/* PFRtgQ1..4  */
    set_s16(M120 + 16, 0);       /* PFRtg_SF          */
    set_u16(M120 + 17, U16_NA);  /* WHRtg             */
    set_s16(M120 + 18, 0);       /* WHRtg_SF          */
    set_u16(M120 + 19, U16_NA);  /* AhrRtg            */
    set_s16(M120 + 20, 0);       /* AhrRtg_SF         */
    set_u16(M120 + 21, U16_NA);  /* MaxChaRte         */
    set_s16(M120 + 22, 0);       /* MaxChaRte_SF      */
    set_u16(M120 + 23, U16_NA);  /* MaxDisChaRte      */
    set_s16(M120 + 24, 0);       /* MaxDisChaRte_SF   */
    set_u16(M120 + 25, 0);       /* Pad               */

    /* End marker */
    set_u16(END_HDR, 0xFFFF);
    set_u16(END_HDR + 1, 0);

    s_last_us = esp_timer_get_time();
}

void sunspec_update(void)
{
    ps_values_t v;
    bool valid = ps_data_get(&v);

    char sn[20];
    ps_data_get_serial(sn, sizeof(sn));
    set_str(M1_DATA + 48, 16, sn);

    if (!valid)
        memset(&v, 0, sizeof(v));

    /* Lifetime Wh: integrate inverter output power over elapsed time. */
    int64_t now = esp_timer_get_time();
    double hours = (double)(now - s_last_us) / 3.6e9;
    s_last_us = now;
    s_wh_accum += (v.inv_watts / 10.0) * hours;   /* inv_watts is deci-watt */

    int32_t dc_a = v.pv1_cur + v.pv2_cur;                          /* deci-amp  */
    int32_t dc_v = v.pv1_volt > v.pv2_volt ? v.pv1_volt : v.pv2_volt; /* deci-volt */
    int32_t dc_w = v.pv1_watts + v.pv2_watts;                      /* deci-watt */

    /* AC current */
    set_u16(M101 + 0, (uint16_t)v.inv_cur);  /* A (mA)   */
    set_u16(M101 + 1, (uint16_t)v.inv_cur);  /* AphA     */
    set_u16(M101 + 2, U16_NA);
    set_u16(M101 + 3, U16_NA);
    set_s16(M101 + 4, -3);                   /* A_SF     */
    /* line-line voltages n/a */
    set_u16(M101 + 5, U16_NA);
    set_u16(M101 + 6, U16_NA);
    set_u16(M101 + 7, U16_NA);
    /* phase voltage */
    set_u16(M101 + 8, (uint16_t)v.inv_volt); /* PhVphA (deci-volt) */
    set_u16(M101 + 9, U16_NA);
    set_u16(M101 + 10, U16_NA);
    set_s16(M101 + 11, -1);                  /* V_SF */
    /* power / frequency */
    set_s16(M101 + 12, (int16_t)v.inv_watts);/* W (deci-watt) */
    set_s16(M101 + 13, -1);                  /* W_SF */
    set_u16(M101 + 14, (uint16_t)v.inv_freq);/* Hz (deci-Hz) */
    set_s16(M101 + 15, -1);                  /* Hz_SF */
    /* VA / VAr / PF not implemented (valid SF) */
    set_s16(M101 + 16, S16_NA); set_s16(M101 + 17, 0);
    set_s16(M101 + 18, S16_NA); set_s16(M101 + 19, 0);
    set_s16(M101 + 20, S16_NA); set_s16(M101 + 21, 0);
    /* lifetime Wh */
    set_acc32(M101 + 22, (uint32_t)s_wh_accum);
    set_s16(M101 + 24, 0);                   /* WH_SF */
    /* DC side */
    set_u16(M101 + 25, (uint16_t)dc_a);      /* DCA */
    set_s16(M101 + 26, -1);                  /* DCA_SF */
    set_u16(M101 + 27, (uint16_t)dc_v);      /* DCV */
    set_s16(M101 + 28, -1);                  /* DCV_SF */
    set_s16(M101 + 29, (int16_t)dc_w);       /* DCW */
    set_s16(M101 + 30, -1);                  /* DCW_SF */
    /* temperatures */
    set_s16(M101 + 31, S16_NA);              /* TmpCab */
    set_s16(M101 + 32, (int16_t)v.inv_temp); /* TmpSnk (deci-C) */
    set_s16(M101 + 33, S16_NA);              /* TmpTrns */
    set_s16(M101 + 34, S16_NA);              /* TmpOt */
    set_s16(M101 + 35, -1);                  /* Tmp_SF */
    /* operating state + events */
    set_u16(M101 + 36, v.inv_watts > 0 ? 4 : 2);  /* St: 4=MPPT, 2=Sleeping */
    set_u16(M101 + 37, 0);                   /* StVnd */
    for (int i = 38; i <= 49; i++) set_u16(M101 + i, 0);  /* Evt1/2 + vendor */
}

int sunspec_read(uint16_t start, uint16_t count, uint16_t *out)
{
    if (count == 0 || (uint32_t)start + count > SUNSPEC_NUM_REGS)
        return 0;
    for (uint16_t i = 0; i < count; i++)
        out[i] = s_regs[start + i];
    return count;
}
