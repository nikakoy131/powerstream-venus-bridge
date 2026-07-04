# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project goal

Bridge an EcoFlow PowerStream micro-inverter to Victron Cerbo GX / Venus OS using an ESP32-C6 as a protocol translator:

```
PowerStream ──BLE──► ESP32-C6 ──WiFi──► Cerbo GX (Venus OS)
                  (this firmware)      appears as PV inverter
```

The ESP32-C6 connects to the PowerStream over BLE (central role), decodes its proprietary AES-encrypted protobuf protocol, and re-exposes the inverter data as a SunSpec Modbus-TCP server on port 502. Venus OS auto-detects it via its built-in `dbus-fronius` driver. Full protocol details are in `docs/RESEARCH.md`.

## Build system

**PlatformIO 6.1.19 + ESP-IDF 6.0.1 framework** via platform `espressif32@^7.0.0`.  
Board: `esp32-c6-devkitc-1` (actual chip: ESP32-C6FH4, 4 MB embedded flash).  
Device appears at `/dev/ttyACM0` (USB-JTAG/CDC).

```bash
pio run                   # build
pio run -t upload         # flash over /dev/ttyACM0
```

There are no tests. Serial monitor via PlatformIO is broken in non-interactive shells; use Python instead:

```bash
~/.platformio/penv/bin/python -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3)
s.setRTS(True); time.sleep(0.15); s.setRTS(False)   # reset board
end = time.time() + 15
while time.time() < end:
    data = s.read(2048)
    if data: print(data.decode(errors='replace'), end='', flush=True)
s.close()
"
```

## Toolchain quirks

- **PlatformIO Python env** lives at `~/.platformio/penv` (Python 3.12 via uv). It is symlinked to `~/.local/bin/pio`. If it breaks again (system Python upgrade), rebuild with `uv venv --python 3.12 --seed ~/.platformio/penv && ~/.platformio/penv/bin/pip install -U platformio intelhex`.
- **`sdkconfig.defaults`** is read by kconfgen but cannot override keys already set in `sdkconfig.esp32-c6-devkitc-1` (the persisted config). Edit `sdkconfig.esp32-c6-devkitc-1` directly for one-off changes; after any sdkconfig edit, delete `.pio/build/esp32-c6-devkitc-1/` before rebuilding.
- **Flash size in binary header** is set by `board_upload.flash_size` / `board_build.flash_size` in `platformio.ini`, not by sdkconfig alone.
- **`ESP_LOGI` and friends** are compiled away in release builds — any variable used only inside an `ESP_LOG*` call will trigger `-Werror=unused-variable`. Use `(void)var` or restructure.
- **cJSON is not available** in ESP-IDF 6.x. JSON is written manually in `ble_scan.c`.
- **`WIFI_AUTH_WPA2_PSK` on C6 requires** `pairwise_cipher = WIFI_CIPHER_TYPE_CCMP` to be set explicitly; the default of 0 produces a malformed RSN IE that causes all clients to refuse association.
- **BLE + WiFi coexistence**: the C6 has a single shared 2.4 GHz radio. BLE scan *must* be duty-cycled (`window < itvl`) or it starves WiFi association. Current params: `itvl=160` (100 ms), `window=48` (30 ms), ~30% duty cycle.

## Current firmware (milestone 1 — BLE scanner)

`src/main.c` initialises NVS, then calls three subsystems in order:

1. **`wifi_apsta.c`** — brings up WiFi in AP+STA mode. AP: `PowerStream-Bridge` / `powerstream` at `192.168.4.1`. STA: joins home network if `src/wifi_secrets.h` has credentials (gitignored; copy from `wifi_secrets.h.example`).

2. **`ble_scan.c`** — NimBLE central, duty-cycled active scan. Maintains a 40-slot table of `{mac, name, rssi, last_seen_s}` behind a FreeRTOS mutex. Exposes `ble_scan_results_json(buf, len)` for the HTTP layer.

3. **`http_server.c`** — serves `GET /` (HTML from `src/index_html.h`, an inlined C string) and `GET /api/devices` (JSON from `ble_scan_results_json`). The web UI polls every 2 s and highlights rows whose name starts with `HW51` or `EF-HW` (PowerStream identifiers).

Thread safety: BLE callbacks write the device table; HTTP handlers read it. Access is always under `s_mutex` in `ble_scan.c`.

## Milestone 2 — BLE decode (DONE)

The bridge connects to the PowerStream, decrypts its protocol, decodes the
`inverter_heartbeat`, and serves live values at `GET /api/powerstream` + a panel
in the web UI. Verified streaming live data (PV voltage, temps, load setpoint).

**Critical protocol correction:** this PowerStream firmware uses **`encrypt_type = 1`,
not the `encrypt_type = 7` (SECP160r1 ECDH) path that `docs/RESEARCH.md` assumed.**
Type 1 has **no ECDH and no `genSessionKey`/keydata table**:

- `session_key = MD5(dev_sn)`, `iv = MD5(reversed dev_sn)`, AES-128-CBC.
- Wire framing is **RawHeader** (`src/rawframe.c`), *not* the `5A5A` EncPacket: the
  inner packet's 5-byte plaintext header (`AA ver lenLE crc8`) + AES-CBC(zero-padded
  body). No pubkey/key-info exchange — just subscribe → send auth_status
  (`0x35/0x89`) → send auth (`0x35/0x86`, `MD5(user_id+dev_sn)` upper-hex) → stream.
- `encrypt_type` came out wrong because the C6's legacy BLE scan only sees the stub
  `0xC5C5` advert (→ defaults to 7); the real `0xB5B5` advert (seen by BlueZ, and by
  the C6 in a different DISC event) carries `capability_flags` (→ type 1) and the
  full 16-char serial. We now parse the serial from the `0xB5B5` advert.

**Multi-device:** the full serial is auto-detected from each unit's `0xB5B5`
advert (`store_serial`/`lookup_serial` in `ble_scan.c`), so any PowerStream on the
account works with no per-device config.

**Runtime config (no hardcoded secrets):** WiFi STA credentials, the EcoFlow
`user_id`, and the AP settings (`ap_ssid`/`ap_pass`/`ap_enabled`) are stored in NVS
and edited from the web UI **Settings** tab (`settings.c`, `GET`/`POST
/api/settings`; saving reboots). The gitignored headers `wifi_secrets.h` /
`ps_config.h` are *optional* dev seeds pulled in via `__has_include` — a fresh
checkout builds without them (AP-only until configured). On first boot with no WiFi
set, connect to the `PowerStream-Bridge` AP (`192.168.4.1`) and configure. The AP
can be disabled, but a fallback brings it back after `AP_FALLBACK_RETRIES` failed
STA reconnects (~1 min) and drops it again on STA connect (`wifi_apsta.c`); with no
STA configured the AP always runs. Empty `ap_pass` = open AP (stored empty in NVS —
`load_key_opt`), and the POST handler rejects 1–7 char AP passwords.

**Web UI:** `src/web/index.html` is the source; `src/index_html.h` is **generated**
from it by `python3 src/web/genheader.py` — edit the HTML, rerun the script, never
edit the header by hand.

**Module map:** `crypto.c` (MD5/AES-CBC/CRC16-ARC/CRC8), `rawframe.c` (type-1
framing + reassembly), `packet.c` (inner V2/V3 packet, seq[0] XOR unmask, sentinel),
`ps_proto.c` (hand-rolled `inverter_heartbeat` walker, field numbers in the file),
`ps_data.c` (latest values behind a mutex + JSON). The BLE central + handshake
state machine lives in `ble_scan.c`. The device allows **one BLE connection** at a
time, so the bridge competes with the EcoFlow phone app for the slot.

## Milestone 3 — SunSpec Modbus-TCP (DONE, pending Venus verification)

`sunspec.c` builds the 152-register map (SunS marker, models 1/101/120, end
marker) and `sunspec_update()` refreshes model 101 from the latest heartbeat with
correct SF scaling (A_SF=-3, V/W/Hz/DC/Tmp_SF=-1). `modbus.c` is a minimal
Modbus-TCP server on port 502 (FC3 only, unit id 126) started from `main.c`.
PV voltage maps to the model-101 **DCV** register. Verified end-to-end with a host
Modbus client (correct marker, model headers, lengths, live values). **Not yet
confirmed against a real Cerbo GX** — that's the remaining acceptance test.

Lifetime Wh is accumulated in RAM (`s_wh_accum` in `sunspec.c`) and resets on
reboot.

## Remaining milestones

- **Venus OS acceptance** — confirm `dbus-fronius` auto-detects the device as
  `com.victronenergy.pvinverter` on a real Cerbo GX.
- **NVS persistence** — persist lifetime Wh across reboots.

Key external references: [ha-ef-ble](https://github.com/rabits/ha-ef-ble) (BLE protocol; note its `encrypt_type` is read from the advert's `capability_flags`), [nanopb](https://github.com/nanopb/nanopb).
