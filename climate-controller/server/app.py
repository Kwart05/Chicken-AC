"""
Chicken AC — Climate Controller Backend
FastAPI server bridging Arduino Nano USB serial ↔ REST/WebSocket ↔ browser.

Usage:
    python app.py --port COM3            (Windows)
    python app.py --port /dev/ttyUSB0   (Linux/RPi)
    python app.py --port DEMO           (no hardware; runs a simulated sensor loop)

CLI args:
    --port      Serial port path (required). Use DEMO for simulation mode.
    --baud      Baud rate (default 115200)
    --host      Bind host (default 0.0.0.0)
    --web-port  HTTP port (default 8000)
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import os
import random
import sqlite3
import threading
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

import serial
import serial.serialutil
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("chicken_ac")

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
STATIC_DIR = Path(__file__).parent / "static"
try:
    DB_PATH = Path(__file__).parent / "climate.db"
    if not os.access(DB_PATH.parent, os.W_OK):
        DB_PATH = Path("/tmp/climate.db")
except Exception:
    DB_PATH = Path("/tmp/climate.db")

# ---------------------------------------------------------------------------
# Shared state (protected by a threading.Lock for serial thread writes;
# asyncio tasks only read, and they run in the event loop thread)
# ---------------------------------------------------------------------------
_state_lock = threading.Lock()
_state: dict[str, Any] = {
    "connected":    False,
    "temperature":  None,
    "humidity":     None,
    "pump":         False,
    "fan":          False,
    "mode":         "AUTO",
    "water_ok":     True,
    "temp_threshold": 30.0,
    "hum_threshold":  70.0,
    "ts":           None,
}

# Set by main() after arg parsing
_serial_port: str = ""
_baud_rate:   int = 9600

# Serial port handle (written by serial thread, read by command senders)
_ser_handle: serial.Serial | None = None
_ser_lock = threading.Lock()

# WebSocket connection registry
_ws_clients: set[WebSocket] = set()
_ws_lock = asyncio.Lock()

# Event loop reference (set in lifespan) so the serial thread can schedule
# coroutines into it.
_loop: asyncio.AbstractEventLoop | None = None

# ---------------------------------------------------------------------------
# Database
# ---------------------------------------------------------------------------
DB_WRITE_INTERVAL = 30  # seconds between writes

def init_db() -> None:
    con = sqlite3.connect(DB_PATH)
    con.execute("""
        CREATE TABLE IF NOT EXISTS readings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          REAL    NOT NULL,
            temperature REAL,
            humidity    REAL,
            pump        INTEGER NOT NULL DEFAULT 0,
            fan         INTEGER NOT NULL DEFAULT 0,
            mode        TEXT    NOT NULL DEFAULT 'AUTO',
            water_ok    INTEGER NOT NULL DEFAULT 1
        )
    """)
    con.execute("CREATE INDEX IF NOT EXISTS idx_ts ON readings (ts)")
    con.commit()
    con.close()
    log.info("Database ready: %s", DB_PATH)


def write_reading(state: dict[str, Any]) -> None:
    """Insert current state snapshot into SQLite (called from serial thread)."""
    try:
        con = sqlite3.connect(DB_PATH)
        con.execute(
            """INSERT INTO readings
               (ts, temperature, humidity, pump, fan, mode, water_ok)
               VALUES (?, ?, ?, ?, ?, ?, ?)""",
            (
                state["ts"] or time.time(),
                state["temperature"],
                state["humidity"],
                1 if state["pump"]     else 0,
                1 if state["fan"]      else 0,
                state["mode"],
                1 if state["water_ok"] else 0,
            ),
        )
        con.commit()
        con.close()
    except Exception as exc:
        log.warning("DB write failed: %s", exc)


# ---------------------------------------------------------------------------
# Serial / Demo thread
# ---------------------------------------------------------------------------
def _parse_nano_line(line: str) -> dict[str, Any] | None:
    """Parse one JSON line from the Nano into a state dict."""
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        return None

    return {
        "temperature":     d.get("t"),
        "humidity":        d.get("h"),
        "pump":            bool(d.get("p", 0)),
        "fan":             bool(d.get("f", 0)),
        "mode":            d.get("m", "AUTO"),
        "water_ok":        bool(d.get("w", 1)),
        "temp_threshold":  float(d.get("tt", 30.0)),
        "hum_threshold":   float(d.get("ht", 70.0)),
    }


def _apply_update(update: dict[str, Any]) -> None:
    """Merge parsed Nano data into shared state and schedule a WS broadcast."""
    with _state_lock:
        _state.update(update)
        _state["connected"] = True
        _state["ts"] = time.time()
        snapshot = dict(_state)

    if _loop is not None:
        asyncio.run_coroutine_threadsafe(_broadcast(snapshot), _loop)


# Command queue for WiFi mode (ESP8266 fetches commands on POST /api/telemetry)
_wifi_cmd_queue: list[dict[str, Any]] = []
_wifi_queue_lock = threading.Lock()

def _queue_wifi_command(cmd: dict[str, Any]) -> None:
    with _wifi_queue_lock:
        _wifi_cmd_queue.append(cmd)

def _pop_wifi_commands() -> list[dict[str, Any]]:
    with _wifi_queue_lock:
        cmds = list(_wifi_cmd_queue)
        _wifi_cmd_queue.clear()
        return cmds

def serial_thread() -> None:
    """Background thread: open serial, read lines, auto-reconnect on failure."""
    global _ser_handle

    if _serial_port == "DEMO":
        demo_thread()
        return
    elif _serial_port == "WIFI":
        log.info("WIFI mode — waiting for HTTP telemetry from ESP8266 on /api/telemetry.")
        return

    last_db_write = 0.0

    while True:
        try:
            log.info("Opening serial port %s @ %d baud…", _serial_port, _baud_rate)
            ser = serial.Serial(_serial_port, _baud_rate, timeout=2)
            with _ser_lock:
                _ser_handle = ser

            log.info("Serial connected.")
            with _state_lock:
                _state["connected"] = True

            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue

                update = _parse_nano_line(line)
                if update is None:
                    continue

                _apply_update(update)

                now = time.time()
                if now - last_db_write >= DB_WRITE_INTERVAL:
                    with _state_lock:
                        snap = dict(_state)
                    write_reading(snap)
                    last_db_write = now

        except serial.serialutil.SerialException as exc:
            log.warning("Serial error: %s — reconnecting in 5 s…", exc)
            with _ser_lock:
                _ser_handle = None
            with _state_lock:
                _state["connected"] = False
            if _loop is not None:
                with _state_lock:
                    snap = dict(_state)
                asyncio.run_coroutine_threadsafe(_broadcast(snap), _loop)
            time.sleep(5)

        except Exception as exc:
            log.error("Unexpected serial thread error: %s", exc)
            time.sleep(5)


def demo_thread() -> None:
    """Simulate sensor readings without physical hardware."""
    global _ser_handle

    log.info("DEMO mode — generating simulated sensor readings.")
    last_db_write = 0.0
    t = time.time()

    while True:
        elapsed = time.time() - t
        temp = 27.0 + 5.0 * math.sin(elapsed / 60.0) + random.gauss(0, 0.3)
        hum  = 60.0 + 10.0 * math.cos(elapsed / 90.0) + random.gauss(0, 1.0)
        temp = round(max(15.0, min(45.0, temp)), 1)
        hum  = round(max(20.0, min(95.0, hum)), 1)

        with _state_lock:
            tt = _state["temp_threshold"]
            ht = _state["hum_threshold"]
            mode = _state["mode"]

        if mode == "AUTO":
            active = (temp >= tt) or (hum >= ht)
            pump = active
            fan  = active
        else:
            with _state_lock:
                pump = _state["pump"]
                fan  = _state["fan"]

        update = {
            "temperature":    temp,
            "humidity":       hum,
            "pump":           pump,
            "fan":            fan,
            "mode":           mode,
            "water_ok":       True,
            "temp_threshold": tt,
            "hum_threshold":  ht,
        }
        _apply_update(update)

        now = time.time()
        if now - last_db_write >= DB_WRITE_INTERVAL:
            with _state_lock:
                snap = dict(_state)
            write_reading(snap)
            last_db_write = now

        time.sleep(2)


# ---------------------------------------------------------------------------
# WebSocket broadcast
# ---------------------------------------------------------------------------
async def _broadcast(state: dict[str, Any]) -> None:
    payload = json.dumps(state)
    dead: set[WebSocket] = set()
    async with _ws_lock:
        for ws in _ws_clients:
            try:
                await ws.send_text(payload)
            except Exception:
                dead.add(ws)
        _ws_clients.difference_update(dead)


# ---------------------------------------------------------------------------
# Serial command helper
# ---------------------------------------------------------------------------
def _send_command(cmd: dict[str, Any]) -> bool:
    """Write a JSON command line to the Nano/ESP8266. Returns True on success."""
    if _serial_port == "WIFI":
        _queue_wifi_command(cmd)
        return True
    with _ser_lock:
        ser = _ser_handle
    if ser is None and _serial_port != "DEMO":
        return False
    if _serial_port == "DEMO":
        # In demo mode, apply commands directly to shared state
        return True  # commands are handled per-endpoint for demo
    try:
        line = json.dumps(cmd) + "\n"
        ser.write(line.encode("utf-8"))
        return True
    except Exception as exc:
        log.warning("Command write failed: %s", exc)
        return False


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI):
    global _loop
    _loop = asyncio.get_running_loop()

    init_db()

    t = threading.Thread(target=serial_thread, daemon=True)
    t.start()

    yield  # App runs here

    log.info("Shutting down…")


app = FastAPI(title="Chicken AC — Climate Controller", lifespan=lifespan)


# ── REST endpoints ──────────────────────────────────────────────────────────

@app.post("/api/telemetry")
def receive_telemetry(data: dict):
    """
    HTTP endpoint for ESP8266 NodeMCU to post sensor telemetry over WiFi.
    Expects payload: {"t": 28.5, "h": 65.0, "p": 1, "f": 1, "m": "AUTO", "w": 1, "tt": 30.0, "ht": 70.0}
    Returns any pending commands for the ESP8266 to execute: {"cmds": [...]}
    """
    update = _parse_nano_line(json.dumps(data))
    if update:
        _apply_update(update)

    cmds = _pop_wifi_commands()
    return {"ok": True, "cmds": cmds}


# ── REST endpoints ──────────────────────────────────────────────────────────

@app.get("/api/status")
def get_status():
    with _state_lock:
        return dict(_state)


@app.get("/api/history")
def get_history(hours: float = 1.0):
    cutoff = time.time() - hours * 3600
    con = sqlite3.connect(DB_PATH)
    con.row_factory = sqlite3.Row
    rows = con.execute(
        """SELECT ts, temperature, humidity, pump, fan, mode, water_ok
           FROM readings WHERE ts >= ? ORDER BY ts ASC""",
        (cutoff,),
    ).fetchall()
    con.close()
    return [dict(r) for r in rows]


@app.post("/api/mode")
def set_mode(body: dict):
    val = body.get("val", "AUTO")
    if val not in ("AUTO", "MANUAL"):
        return JSONResponse({"error": "val must be AUTO or MANUAL"}, status_code=400)
    cmd = {"cmd": "mode", "val": val}
    with _state_lock:
        _state["mode"] = val
    _send_command(cmd)
    return {"ok": True}


@app.post("/api/pump")
def set_pump(body: dict):
    val = int(body.get("val", 0))
    with _state_lock:
        if _state["mode"] != "MANUAL":
            return JSONResponse({"error": "Switch to MANUAL mode first"}, status_code=409)
    cmd = {"cmd": "pump", "val": val}
    with _state_lock:
        _state["pump"] = bool(val)
    _send_command(cmd)
    return {"ok": True}


@app.post("/api/fan")
def set_fan(body: dict):
    val = int(body.get("val", 0))
    with _state_lock:
        if _state["mode"] != "MANUAL":
            return JSONResponse({"error": "Switch to MANUAL mode first"}, status_code=409)
    cmd = {"cmd": "fan", "val": val}
    with _state_lock:
        _state["fan"] = bool(val)
    _send_command(cmd)
    return {"ok": True}


@app.post("/api/temp-threshold")
def set_temp_threshold(body: dict):
    val = float(body.get("val", 30.0))
    if not (0 < val < 60):
        return JSONResponse({"error": "temp_threshold must be 0–60 °C"}, status_code=400)
    cmd = {"cmd": "temp_thr", "val": val}
    with _state_lock:
        _state["temp_threshold"] = val
    _send_command(cmd)
    return {"ok": True}


@app.post("/api/hum-threshold")
def set_hum_threshold(body: dict):
    val = float(body.get("val", 70.0))
    if not (0 < val <= 100):
        return JSONResponse({"error": "hum_threshold must be 1–100 %"}, status_code=400)
    cmd = {"cmd": "hum_thr", "val": val}
    with _state_lock:
        _state["hum_threshold"] = val
    _send_command(cmd)
    return {"ok": True}


# ── WebSocket ───────────────────────────────────────────────────────────────

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    async with _ws_lock:
        _ws_clients.add(ws)
    log.info("WS client connected. Total: %d", len(_ws_clients))

    # Send current state immediately on connect
    with _state_lock:
        snap = dict(_state)
    await ws.send_text(json.dumps(snap))

    try:
        while True:
            await ws.receive_text()   # keep connection alive; client may ping
    except WebSocketDisconnect:
        pass
    finally:
        async with _ws_lock:
            _ws_clients.discard(ws)
        log.info("WS client disconnected. Total: %d", len(_ws_clients))


# ── Static files (served last so API routes take priority) ──────────────────
if STATIC_DIR.exists():
    app.mount("/", StaticFiles(directory=STATIC_DIR, html=True), name="static")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn

    parser = argparse.ArgumentParser(description="Chicken AC Climate Controller Server")
    parser.add_argument("--port",     required=True,
                        help="Serial port (e.g. COM3, /dev/ttyUSB0, or DEMO)")
    parser.add_argument("--baud",     type=int, default=9600,
                        help="Serial baud rate (default 9600)")
    parser.add_argument("--host",     default="0.0.0.0",
                        help="Bind host (default 0.0.0.0)")
    parser.add_argument("--web-port", type=int, default=8000, dest="web_port",
                        help="HTTP port (default 8000)")
    args = parser.parse_args()

    _serial_port = args.port
    _baud_rate   = args.baud

    log.info("Starting Chicken AC server — serial=%s baud=%d http=%s:%d",
             _serial_port, _baud_rate, args.host, args.web_port)

    uvicorn.run(app, host=args.host, port=args.web_port, log_level="info")
