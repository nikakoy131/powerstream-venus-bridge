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

## Next milestones (from docs/RESEARCH.md)

The scanner milestone validates BLE + WiFi coexistence. Subsequent work:

- **BLE connect & handshake** — when a `HW51`/`EF-HW` device is found, switch from observer to central: ECDH key exchange (SECP160r1 via micro-ecc, NOT mbedTLS), `genSessionKey` lookup table, AES-128-CBC session, MD5 auth packet. See `docs/RESEARCH.md` § BLE protocol.
- **Protobuf decode** — nanopb, `wn511_sys.proto` from ha-ef-ble, `inverter_heartbeat` message.
- **SunSpec Modbus-TCP server** — port 502, unit ID 126, 152 registers, models 1/101/120. Venus OS `dbus-fronius` auto-detects it.
- **NVS persistence** — lifetime Wh accumulation.

Key external references: [ha-ef-ble](https://github.com/rabits/ha-ef-ble) (BLE protocol), [micro-ecc](https://github.com/kmackay/micro-ecc) (SECP160r1), [nanopb](https://github.com/nanopb/nanopb).
