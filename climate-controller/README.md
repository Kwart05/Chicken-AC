# Chicken AC — Climate Controller

> Industrial evaporative-cooling pad controller for poultry warehouses.
> Arduino Nano → USB serial → FastAPI server → browser dashboard.

---

## Architecture

```
┌──────────────────────┐   USB serial (115200 baud)   ┌────────────────────────────┐
│   Arduino Nano       │ ─────── JSON lines ────────▶ │  Python / FastAPI (app.py) │
│                      │ ◀────── JSON commands ─────── │                            │
│  DHT22  (D2)         │                               │  SQLite  climate.db        │
│  Pump relay (D5)     │                               │  REST  /api/*              │
│  Fan relay  (D6)     │                               │  WebSocket  /ws            │
│  Float switch (A0)   │                               │  Static files  /           │
└──────────────────────┘                               └────────────┬───────────────┘
                                                                    │ HTTP + WS
                                                             ┌──────▼──────┐
                                                             │   Browser   │
                                                             │  Dashboard  │
                                                             └─────────────┘
```

---

## Project Structure

```
climate-controller/
  README.md
  arduino/
    climate_controller.ino     ← Nano firmware
  server/
    app.py                     ← FastAPI backend
    requirements.txt
    climate.db                 ← auto-created on first run
    static/
      index.html               ← single-page dashboard
      manifest.json            ← PWA manifest
      service-worker.js        ← offline-first service worker
      icons/
        icon-192.png
        icon-512.png
        icon-512-maskable.png
```

---

## 1. Arduino Setup

### Library Dependencies
Install these via **Arduino IDE → Tools → Manage Libraries**:

| Library | Version tested | Purpose |
|---------|---------------|---------|
| `DHT sensor library` by Adafruit | ≥ 1.4 | DHT22 driver |
| `Adafruit Unified Sensor` | ≥ 1.1 | DHT dependency |
| `ArduinoJson` by Benoit Blanchon | ≥ 7.x | JSON parse/serialize |

### Wiring Summary

| Arduino Pin | Connected to |
|-------------|-------------|
| D2 | DHT22 DATA (+ 10kΩ pull-up to 5V) |
| D5 | Relay module IN1 (Pump, active HIGH) |
| D6 | Relay module IN2 (Fan, active HIGH) |
| A0 | Float switch (other leg to GND; internal pull-up enabled) |
| 5V / GND | Relay VCC/GND, DHT22 VCC/GND |

### Flashing

1. Open `arduino/climate_controller.ino` in the Arduino IDE.
2. Select board: **Arduino Nano**, processor: **ATmega328P** (or Old Bootloader if needed).
3. Select the correct COM port.
4. Click **Upload**.

> **Note:** If your project already has a sketch driving an LCD, merge the logic
> from this file into that sketch rather than uploading this file standalone —
> see the comment block at the top of `.ino` for guidance.

---

## 2. Server Setup

### Requirements

- Python 3.9+
- pip

### Install

```bash
cd server
pip install -r requirements.txt
```

### Run

```bash
# With real hardware
python app.py --port COM3           # Windows
python app.py --port /dev/ttyUSB0  # Linux / Raspberry Pi

# Without hardware (demo / simulation mode)
python app.py --port DEMO

# Full options
python app.py --port COM3 --baud 115200 --host 0.0.0.0 --web-port 8000
```

Open your browser at **http://localhost:8000** (or replace localhost with the
host machine's LAN IP to access from other devices on the same network).

---

## 3. Dashboard Sections

| Section | Description |
|---------|-------------|
| **Header** | System name, connection status LED (green = serial link alive) |
| **Alerts bar** | Live warnings: reservoir empty, serial offline |
| **Sensor Readings** | SVG semicircular gauges for temperature (°C) and humidity (% RH). Red dashed line shows the current trigger threshold on each gauge. |
| **System Status** | Indicator lamps for Water Pump, Vent Fan, and Reservoir level |
| **Controls** | AUTO/MANUAL mode selector; rocker switches (manual pump/fan); threshold sliders. Manual controls are locked out in AUTO mode. |
| **History** | Line chart of temperature + humidity over 1H / 6H / 24H / 7D, sourced from the SQLite database |

---

## 4. Serial JSON Protocol

All communication is newline-terminated single-line JSON at 115200 baud.

### Nano → Host (emitted every ~2 s)

```json
{
  "t":  28.5,
  "h":  65.2,
  "p":  1,
  "f":  1,
  "m":  "AUTO",
  "w":  1,
  "tt": 30.0,
  "ht": 70.0
}
```

| Key | Type | Description |
|-----|------|-------------|
| `t` | float | Temperature °C (1 decimal place) |
| `h` | float | Relative humidity % |
| `p` | 0/1 | Pump relay state |
| `f` | 0/1 | Fan relay state |
| `m` | string | Current mode: `"AUTO"` or `"MANUAL"` |
| `w` | 0/1 | Water OK flag (0 = reservoir empty) |
| `tt` | float | Active temperature threshold °C |
| `ht` | float | Active humidity threshold % |

### Host → Nano (commands)

```json
{ "cmd": "mode",     "val": "MANUAL" }
{ "cmd": "pump",     "val": 1 }
{ "cmd": "fan",      "val": 0 }
{ "cmd": "temp_thr", "val": 32.5 }
{ "cmd": "hum_thr",  "val": 75.0 }
```

**To monitor raw serial traffic** (for debugging):

```bash
# Linux/Mac
screen /dev/ttyUSB0 115200

# Windows — use Arduino IDE Serial Monitor at 115200 baud
```

---

## 5. REST API Reference

| Method | Endpoint | Body / Params | Description |
|--------|----------|--------------|-------------|
| GET | `/api/status` | — | Full current state as JSON |
| GET | `/api/history?hours=N` | N = 1/6/24/168 | Logged readings from SQLite |
| POST | `/api/mode` | `{ "val": "AUTO" }` | Switch AUTO/MANUAL |
| POST | `/api/pump` | `{ "val": 1 }` | Set pump (MANUAL only) |
| POST | `/api/fan` | `{ "val": 1 }` | Set fan (MANUAL only) |
| POST | `/api/temp-threshold` | `{ "val": 30.0 }` | Set temp trigger |
| POST | `/api/hum-threshold` | `{ "val": 70.0 }` | Set humidity trigger |

---

## 6. PWA Installation

The dashboard is a full Progressive Web App and can be installed to a device's home screen.

### On the host machine (localhost)

Chrome/Edge will show an **Install** button in the address bar automatically.
Service workers work on `localhost` without HTTPS.

### On another device on your LAN

Service workers **require HTTPS** (localhost is the only exception).
Two options:

**Option A — ngrok tunnel (quickest)**
```bash
ngrok http 8000
# Use the https://xxx.ngrok.io URL on the remote device
```

**Option B — local HTTPS cert with mkcert**
```bash
# Install mkcert: https://github.com/FiloSottile/mkcert
mkcert -install
mkcert localhost 192.168.x.x  # your LAN IP

# Run with cert (uvicorn directly)
uvicorn app:app --ssl-keyfile localhost+1-key.pem \
                --ssl-certfile localhost+1.pem \
                --host 0.0.0.0 --port 8000
```

---

## 7. Troubleshooting

### "Port is busy" / "Permission denied" on serial port

- **Windows**: Close Arduino IDE Serial Monitor if open. Check Device Manager for the port name.
- **Linux**: Add your user to the `dialout` group: `sudo usermod -aG dialout $USER`, then log out/in.
- If another process has the port: `fuser /dev/ttyUSB0` (Linux) to find and kill it.

### Dashboard shows "DISCONNECTED"

1. Check the serial port argument is correct (`--port COM3` / `--port /dev/ttyUSB0`).
2. Confirm the Nano is powered and connected via USB.
3. Use `--port DEMO` to test the dashboard without hardware.
4. Check server terminal for "Serial error" messages.

### Temperature/humidity shows `--.-`

- The DHT22 takes 2–3 seconds to produce the first valid reading after power-on.
- If it persists: check DHT22 wiring, confirm the pull-up resistor on DATA, and try lowering baud rate to 9600.

### Pump doesn't respond to manual commands

- Ensure the mode is set to **MANUAL** first (the dashboard locks out rocker switches in AUTO mode).
- If the float switch reports the reservoir as empty, the pump is force-disabled regardless of mode.

### Chart shows no data

- History is written to SQLite every 30 seconds. Wait at least 30 s for the first point to appear.
- Click a time-range button to manually reload.

### Dashboard not reachable from another device on the LAN

- Ensure the server is bound to `0.0.0.0` (default), not `127.0.0.1`.
- Check the host machine's firewall: allow TCP inbound on port 8000.
- Windows Firewall: `New-NetFirewallRule -DisplayName "ChickenAC" -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow`
