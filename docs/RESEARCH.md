# Project context — powerstream-venus-bridge

This file summarizes the full design conversation. Paste it into the repo root
so Claude Code has complete context without needing access to claude.ai.

---

## What this project does

Bridge an **EcoFlow PowerStream** micro-inverter to **Victron Cerbo GX / Venus OS**
using an **ESP32-C6** as a protocol translator:

```
PowerStream ──BLE──► ESP32-C6 ──WiFi──► Cerbo GX (Venus OS)
                  (this firmware)      appears as PV inverter
```

Venus OS has no native EcoFlow support. The ESP32-C6 connects to the PowerStream
over BLE (as a central), decodes its proprietary protocol, and re-exposes the data
as a SunSpec Modbus-TCP server on port 502. Venus OS auto-detects it via its
built-in `dbus-fronius` driver as a standard single-phase PV inverter.

---

## Why ESP32-C6 (not C3)

Both were available. C6 chosen for:
- BLE 5.3 vs 4.2 — better connection stability for a central that must stay
  connected long-term (PowerStream heartbeats only every ~30 s)
- Hardware AES/ECC acceleration — firmware does AES-128-CBC on every BLE packet
- Dual RISC-V cores (160 MHz HP + 20 MHz LP) — LP core for Modbus polling,
  HP core for BLE + crypto
- Better WiFi 6 coexistence in congested 2.4 GHz environments

---

## BLE protocol (reverse-engineered from ha-ef-ble)

Source: https://github.com/rabits/ha-ef-ble

### Device identification
- PowerStream advertises with name prefix `HW51` or `EF-HW`
- Serial number starts with `HW51`

### GATT transport
Two possible service pairs (device uses one):
- Nordic UART: write `6e400002-…`, notify `6e400003-…`
- RFCOMM-like: write `00000002-…`, notify `00000003-…`

Notifications may be fragmented — must reassemble before parsing.

### Handshake sequence (encrypt_type = 7)

```
Client                                    Device
  |-- 0x01 0x00 + SECP160r1 pubkey ------->|
  |<-- SECP160r1 pubkey --------------------|
  |   shared = ECDH(our_priv, dev_pub)
  |   key = shared[:16],  iv = MD5(shared)
  |   enc = AES-128-CBC(key, iv)
  |-- cmd 0x02 (request session key) ------>|
  |<-- enc(sRand[16] + seed[2]) ------------|
  |   session_key = genSessionKey(seed, sRand)
  |   enc = AES-128-CBC(session_key, iv)
  |-- auth: MD5(user_id + dev_sn).upper() ->|   32-byte hex ASCII
  |<-- continuous inverter_heartbeat --------|
```

### genSessionKey (critical — must match exactly)

```c
// table[] is the ~7 KB keydata blob (port from keydata.py in ha-ef-ble)
uint16_t pos = seed[0] * 0x10 + ((seed[1] - 1) & 0xFF) * 0x100;
uint64_t data_num[4];
data_num[0] = read_le64(table + pos);
data_num[1] = read_le64(table + pos + 8);
data_num[2] = read_le64(srand + 0);
data_num[3] = read_le64(srand + 8);
uint8_t buf[32];
for (int i = 0; i < 4; i++) write_le64(buf + i*8, data_num[i]);
md5(buf, 32, session_key);  // session_key = 16 bytes
```

### EncPacket frame format

```
[5A][5A][frame_type<<4][01][len:u16LE][AES-CBC payload][crc16:u16LE]
```

- CRC16/ARC: poly=0x8005, init=0, reflect in+out, over full frame incl. `5A 5A`
- Payload decrypted with AES-128-CBC using session key

### Inner Packet

```
[version:u8][src:u8][dst:u8][cmd_set:u8][cmd_id:u8][seq:u16LE][len:u16LE][payload][crc8]
```

PowerStream payload is protobuf, XOR-masked. Proto message: `inverter_heartbeat`
from `wn511_sys.proto` (file is in ha-ef-ble repo).

### Auth credentials

- `user_id`: fetch once from `POST https://api.ecoflow.com/auth/login` — static per account
- `dev_sn`: from BLE advertisement
- Auth payload: `MD5(user_id + dev_sn).hexdigest().upper().encode()` (32 ASCII bytes)

### Keepalive

Send auth_status packet (cmd_set=0x35, cmd_id=0x89, empty payload) every **30 s**
or device stops sending heartbeats.

---

## ESP32-C6 implementation stack

| Task | Library |
|---|---|
| BLE central + notify | NimBLE (ESP-IDF component) |
| SECP160r1 ECDH | **micro-ecc (uECC)** — mbedTLS excludes this curve by default |
| AES-128-CBC | mbedTLS (bundled in ESP-IDF) |
| MD5 | mbedTLS |
| CRC16/ARC | implement directly (~15 lines) |
| CRC8 | implement directly |
| Protobuf decode | **nanopb** — compile only needed messages from wn511_sys.proto |
| keydata table | ~7 KB blob from keydata.py, store in DROM (flash) |
| Modbus-TCP server | implement directly (simple; only FC3 read holding registers needed) |
| Lifetime Wh persistence | NVS (non-volatile storage) |

**Critical:** Do NOT use mbedTLS for SECP160r1. Use micro-ecc.
Add as ESP-IDF component: https://github.com/kmackay/micro-ecc

---

## SunSpec Modbus-TCP server

Port **502**, unit ID **126**, base address **40000** (0-based).
Venus OS `dbus-fronius` driver probes this automatically and registers the device
as `com.victronenergy.pvinverter`.

### Memory layout (152 registers total)

| Address | Content | Length |
|---|---|---|
| 40000 | `SunS` marker = `0x53756E53` | 2 words |
| 40002 | Model 1 header: ID=1, L=66 | 2 words |
| 40004 | Model 1 data (Common, static) | 66 words |
| 40070 | Model 101 header: ID=101, L=50 | 2 words |
| 40072 | Model 101 data (Inverter, live) | 50 words |
| 40122 | Model 120 header: ID=120, L=26 | 2 words |
| 40124 | Model 120 data (Nameplate, static) | 26 words |
| 40150 | End marker: ID=0xFFFF, L=0 | 2 words |

**Model lengths must be exact** (66/50/26) or Venus detection fails silently.

### Scaling — raw protobuf → register (no float math needed)

Every PowerStream field arrives as integer × 10^n, matching SunSpec's SF scheme:

| PowerStream field | Raw unit | Register | SF |
|---|---|---|---|
| `inverter_power` | deci-watt | `W` | `W_SF = -1` |
| `inverter_voltage` | deci-volt | `PhVphA` | `V_SF = -1` |
| `inverter_current` | milli-amp | `A` / `AphA` | `A_SF = -3` |
| `inverter_frequency` | deci-Hz | `Hz` | `Hz_SF = -1` |
| `pv1_watts + pv2_watts` | deci-watt | `DCW` | `DCW_SF = -1` |
| `pv voltage` | deci-volt | `DCV` | `DCV_SF = -1` |
| `pv current` | deci-amp | `DCA` | `DCA_SF = -1` |
| `inverter_temperature` | deci-°C | `TmpSnk` | `Tmp_SF = -1` |

### Key Model 101 offsets (from register 40072)

| Offset | Point | Value |
|---|---|---|
| 0 | A | inverter_current raw |
| 1 | AphA | same |
| 2-3 | AphB/C | 0xFFFF |
| 4 | A_SF | -3 |
| 8 | PhVphA | inverter_voltage raw |
| 9-10 | PhVphB/C | 0xFFFF |
| 11 | V_SF | -1 |
| 12 | W | inverter_power raw (int16) |
| 13 | W_SF | -1 |
| 14 | Hz | inverter_frequency raw |
| 15 | Hz_SF | -1 |
| 22-23 | WH | lifetime Wh (acc32, high word first) |
| 24 | WH_SF | 0 |
| 25 | DCA | pv1_cur + pv2_cur raw |
| 26 | DCA_SF | -1 |
| 27 | DCV | max(pv1_volt, pv2_volt) raw |
| 28 | DCV_SF | -1 |
| 29 | DCW | pv1_watts + pv2_watts raw (int16) |
| 30 | DCW_SF | -1 |
| 32 | TmpSnk | inverter_temperature raw |
| 35 | Tmp_SF | -1 |
| 36 | St | 4=MPPT (W>0), 2=Sleeping |

Unimplemented: uint16 → `0xFFFF`, int16 → `0x8000`. Never use sentinel for SF registers.

### Model 1 — Common (static strings, big-endian packed, 2 chars/register)

| Offset | Point | Value |
|---|---|---|
| 0 | Mn | "EcoFlow" (16 words) |
| 16 | Md | "PowerStream" (16 words) |
| 48 | SN | device serial HW51… (16 words) |
| 64 | DA | 126 |

### Model 120 — Nameplate (static)

| Offset | Point | Value |
|---|---|---|
| 0 | DERTyp | 4 (PV) |
| 1 | WRtg | 800 (or 600 for 600W model) |
| 2 | WRtg_SF | 0 |

---

## Firmware architecture

```
main/
├── main.c
├── ble/
│   ├── ble_client.c/h      # NimBLE central, scan, connect, notify subscribe
│   ├── handshake.c/h       # uECC ECDH, genSessionKey, auth packet
│   ├── frame.c/h           # EncPacket framing, CRC16/ARC, AES-CBC
│   └── packet.c/h          # Inner packet, CRC8, XOR unmask
├── proto/
│   ├── wn511_sys.proto     # source (copy from ha-ef-ble)
│   ├── wn511_sys.pb.c      # nanopb generated
│   └── wn511_sys.pb.h
├── sunspec/
│   ├── registers.c/h       # register array + update() from decoded fields
│   └── server.c/h          # Modbus-TCP server, FC3 handler
├── keydata.c/h             # 7KB lookup table ported from keydata.py
└── nvs_storage.c/h         # persist lifetime Wh
```

### Thread safety
BLE callback writes registers; Modbus-TCP handler reads them.
Use `SemaphoreHandle_t` mutex or double-buffer around register array.

### Lifetime Wh
```c
wh_accum += (inverter_power_raw / 10.0f) * ((now_us - last_us) / 3.6e9f);
// persist to NVS every 60s
```

### BLE reconnect
On disconnect: wait 10s → re-scan for HW51 prefix → full ECDH handshake again.
Exponential backoff up to 5 min on repeated failures.

---

## Reference projects

- **ha-ef-ble** https://github.com/rabits/ha-ef-ble — BLE protocol source of truth.
  Read: `eflib/connection.py`, `eflib/encryption.py`, `eflib/encpacket.py`,
  `eflib/keydata.py`, `eflib/devices/powerstream.py`
- **sunspecbridge** https://github.com/izak/sunspecbridge — MicroPython SunSpec
  server reference for the Venus-facing half
- **micro-ecc** https://github.com/kmackay/micro-ecc — SECP160r1 on ESP32
- **nanopb** https://github.com/nanopb/nanopb — protobuf for embedded C

---

## Venus OS integration notes

- Venus OS `dbus-fronius` auto-detects SunSpec on LAN, no config needed
- Device appears as `com.victronenergy.pvinverter`
- For zero feed-in / ESS control later: implement SunSpec model 123 or 704
- Cerbo GX onboard BT is weak — running BLE on a dedicated ESP32 is the right call