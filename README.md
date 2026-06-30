# PowerStream → Venus OS Bridge

Bridge an **EcoFlow PowerStream** micro-inverter to a **Victron Cerbo GX / Venus OS**
system using an **ESP32-C6** as a protocol translator.

```
PowerStream ──BLE──► ESP32-C6 ──WiFi──► Cerbo GX (Venus OS)
                  (this firmware)      appears as a PV inverter
```

The ESP32-C6 connects to the PowerStream over Bluetooth LE (central role), decrypts
its proprietary AES-encrypted protobuf protocol, and re-exposes the live inverter
data as a **SunSpec Modbus-TCP** server on port 502. Venus OS auto-detects it through
its built-in `dbus-fronius` driver, so the PowerStream shows up natively as a
PV inverter — no custom Venus packages required.

A small web UI (served on port 80) shows the BLE scan, live decoded values, and a
settings page for WiFi + account credentials.

## What it does

- Scans for nearby PowerStream units over BLE and connects to them.
- Auto-detects each unit's serial number from its advertisement — **no per-device
  configuration**; any PowerStream on your account works.
- Decrypts and decodes the `inverter_heartbeat` telemetry (PV, inverter/AC, battery,
  temperatures, load setpoint).
- Serves the data three ways:
  - **Web UI** — `http://<bridge-ip>/` (live dashboard + settings).
  - **JSON API** — `GET /api/powerstream`, `GET /api/devices`.
  - **Modbus-TCP / SunSpec** — port 502, unit id 126 (for Venus OS).

## Usage scenarios

- **Victron / Venus OS users** who want PowerStream PV production visible in VRM and on
  the Cerbo GX without the EcoFlow cloud — the bridge feeds Venus locally over Modbus.
- **Local monitoring** — a self-contained dashboard of live PowerStream values on your
  LAN, no app or internet round-trip.
- **Multiple PowerStreams** on one account — the bridge picks them up by advertisement,
  nothing to hardcode.

> Note: the PowerStream allows **one BLE connection at a time**, so while the bridge is
> connected the EcoFlow phone app cannot be (and vice-versa). The app's cloud link keeps
> working independently.

## Hardware

- **ESP32-C6** dev board (developed on `esp32-c6-devkitc-1`, chip ESP32-C6FH4, 4 MB flash).
  The C6's single shared 2.4 GHz radio runs BLE + WiFi concurrently (BLE scan is
  duty-cycled so WiFi keeps working).
- USB-C cable. The board enumerates as `/dev/ttyACM0` (USB-JTAG/CDC).

## Build & flash

Built with **PlatformIO** (ESP-IDF 6.x framework). With PlatformIO installed:

```bash
pio run                   # build
pio run -t upload         # flash over /dev/ttyACM0
```

To watch the serial log (the PlatformIO monitor is unreliable in non-interactive
shells; this Python snippet is reliable):

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

No secrets are baked into the build — a fresh checkout builds and runs as-is, then is
configured at runtime (see below).

## First-time setup / how to connect

1. **Flash** the firmware (above).
2. On boot, with no WiFi configured, the bridge starts a WiFi access point:
   - SSID: **`PowerStream-Bridge`**
   - Password: **`powerstream`**
   - Bridge address: **`http://192.168.4.1`**
3. Connect to that AP and open **`http://192.168.4.1`** → **Settings** tab. Enter:
   - **WiFi network (STA SSID)** + password — your home/LAN WiFi, so the bridge joins
     the same network as your Cerbo GX. (Leave blank to stay AP-only.)
   - **EcoFlow account user_id** — needed for BLE authentication (see below).
4. Save & reboot. The bridge joins your WiFi; find its new IP from your router (or it
   logs the IP on serial). Open `http://<that-ip>/` — the **Dashboard** shows the BLE
   scan and, once connected and authenticated, live PowerStream values.
5. **Venus OS**: ensure the Cerbo GX is on the same network. `dbus-fronius` scans for
   SunSpec devices automatically; the PowerStream should appear as a PV inverter
   (`com.victronenergy.pvinverter`). If auto-detection is off, enable Modbus-TCP device
   scanning in the Cerbo GX settings.

## How to get your EcoFlow user_id

BLE authentication uses `MD5(user_id + device_serial)` — the device serial is detected
automatically, so the only credential you supply is your account **user_id**.

The easiest way to get it is the community helper:

**→ [gnox.github.io/user_id](https://gnox.github.io/user_id)**

Enter your EcoFlow app login once; it queries the EcoFlow API and returns your numeric
user_id. Your credentials are used only for that single request and are not stored. This
same link is also shown on the bridge's **Settings** tab.

(Under the hood the user_id comes from `POST https://api.ecoflow.com/auth/login`; it is
static per account, so you fetch it once.)

## How it works (brief)

The PowerStream firmware used here speaks the **`encrypt_type = 1`** variant of EcoFlow's
BLE protocol:

- `session_key = MD5(dev_sn)`, `iv = MD5(reversed dev_sn)`, AES-128-CBC.
- Wire framing is the plaintext 5-byte inner header (`AA ver lenLE crc8`) + AES-CBC body.
- Handshake: subscribe → send `auth_status` → send `auth` (`MD5(user_id+dev_sn)` upper-hex)
  → telemetry streams. A keepalive is sent every 30 s.

Decoded heartbeats feed a SunSpec register map (models 1 / 101 / 120) served over
Modbus-TCP. Full protocol notes are in [`docs/RESEARCH.md`](docs/RESEARCH.md); module-by-module
architecture is in [`CLAUDE.md`](CLAUDE.md).

## Credits / references

- [rabits/ha-ef-ble](https://github.com/rabits/ha-ef-ble) — EcoFlow BLE protocol reference.
- [gnox.github.io/user_id](https://gnox.github.io/user_id) — EcoFlow user_id finder.
- SunSpec Modbus + Victron `dbus-fronius` for PV-inverter auto-detection.
