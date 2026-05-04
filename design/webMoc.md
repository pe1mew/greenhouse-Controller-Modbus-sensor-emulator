# Web Mock Server — Design Plan

**Purpose**: A local Python/Flask server that serves the same `firmware/data/`
web assets and exposes every HTTP and WebSocket endpoint that
`firmware/sensorEmulator/web/web_server.cpp` provides on the Atom Lite.  The
mock lets the web UI be developed, tested, and demoed entirely on a desktop PC
without flashing any firmware.

---

## 1  Scope

| What is mocked | Notes |
|----------------|-------|
| `GET /`, `/index.html`, `/style.css`, `/app.js` | Served directly from `firmware/data/` |
| `GET /ws` | WebSocket; pushes a `status` JSON every 1 s |
| `POST /config/sensor` | Clamping, state update, NVS-equivalent in-process dict |
| `POST /config/wifi` | Accepts and logs; updates simulated WiFi state |
| `POST /config/time` | Parses ISO-8601, updates simulated clock offset |
| `POST /config/ntp` | Saves NTP server name in in-process settings dict |
| `POST /replay/upload` | Returns `501 Not Implemented` (matches Phase 11 stub) |
| `POST /replay/control` | Returns `501 Not Implemented` (matches Phase 11 stub) |
| `POST /log/clear` | Broadcasts `{type:"log_clear"}` WS message |
| 1 Hz simulated log entries | Pushes a fake `{type:"log", …}` frame to exercise UI log table |

What is **not** mocked: actual Modbus RS485 traffic, NVS persistence across
process restarts (state is in-memory only), real NTP sync.

---

## 2  Directory Layout

```
webMoc/
├── server.py          ← Flask + Flask-Sock application
├── requirements.txt   ← flask, flask-sock
└── README.md          ← how to install and run
```

The mock does **not** copy the web assets; it imports them directly from
`../firmware/data/` using Flask's `static_folder` parameter so every UI edit
is reflected immediately without restarting the server.

---

## 3  Dependencies

```
flask>=3.0
flask-sock>=0.7          # WebSocket support via simple-websocket
```

`flask-sock` wraps the standard `simple-websocket` library and exposes a
`@sock.route` decorator that closely mirrors the ESP-IDF WebSocket pattern.

Install:
```
pip install -r webMoc/requirements.txt
```

Run:
```
python webMoc/server.py
```
Server listens on `http://127.0.0.1:5000` by default.

---

## 4  Simulated Device State

A single module-level dict `_state` holds all mutable device state.  It is
**thread-safe via a `threading.Lock`** (Flask's development server is
multi-threaded by default when `threaded=True`).

```python
_state = {
    # FG6485A
    "fg_addr":        1,
    "fg_mode":        0,           # 0=MANUAL, 1=LIVE, 2=REPLAY
    "fg_temp_raw":    250,         # int16, raw = °C × 10
    "fg_hum_raw":     500,         # uint16, raw = %RH × 10
    # S200
    "s200_addr":      44,
    "s200_mode":      0,
    "s200_spd_raw":   5000,        # int32, raw = m/s × 1000
    "s200_dir_raw":   180000,      # int32, raw = ° × 1000
    "s200_heat_raw":  25000,       # int32, raw = °C × 1000
    # WiFi
    "wifi_mode":      "AP",        # AP | Connecting | STA
    "wifi_ip":        "192.168.4.1",
    "wifi_rssi":      0,
    "wifi_ssid":      "",
    # Clock
    "time_offset_s":  0,           # seconds added to time.time() for manual-set simulation
    "ntp_synced":     False,
    "ntp_server":     "pool.ntp.org",
}
_state_lock = threading.Lock()
```

---

## 5  Physical Range Constants (mirrors `sensor_state.h`)

```python
FG_TEMP_RAW_MIN,  FG_TEMP_RAW_MAX  = -400,    1200
FG_HUM_RAW_MIN,   FG_HUM_RAW_MAX   =    0,     999
S200_SPD_RAW_MIN, S200_SPD_RAW_MAX =    0,   60000
S200_DIR_RAW_MIN, S200_DIR_RAW_MAX =    0,  360000
S200_HEAT_RAW_MIN,S200_HEAT_RAW_MAX= -40000, 85000
```

Clamping helper (mirrors `clamp_int` in `web_server.cpp`):
```python
def clamp(v, lo, hi):
    return max(lo, min(hi, v))
```

---

## 6  Static File Routes

Flask serves `firmware/data/` as the static folder.

```python
from flask import Flask, send_from_directory

DATA_DIR = os.path.join(os.path.dirname(__file__), '..', 'firmware', 'data')
app = Flask(__name__, static_folder=DATA_DIR, static_url_path='')

@app.route('/')
@app.route('/index.html')
def index():
    return send_from_directory(DATA_DIR, 'index.html')

@app.route('/style.css')
def css():
    return send_from_directory(DATA_DIR, 'style.css')

@app.route('/app.js')
def js():
    return send_from_directory(DATA_DIR, 'app.js')
```

---

## 7  WebSocket Route (`GET /ws`)

Flask-Sock exposes a synchronous per-connection handler; the 1 Hz push is
implemented with a background thread that keeps a **set of active `ws`
objects** protected by its own lock.

```
_ws_clients: set[simple_websocket.Server]
_ws_lock: threading.Lock
```

### 7.1  Connection handler

```python
@sock.route('/ws')
def ws_handler(ws):
    with _ws_lock:
        _ws_clients.add(ws)
    try:
        while True:
            msg = ws.receive(timeout=5)   # blocks; timeout avoids dead-lock on disconnect
            if msg is None:
                break
            # incoming frames discarded (push-only in Phase 7, matching firmware)
    except Exception:
        pass
    finally:
        with _ws_lock:
            _ws_clients.discard(ws)
```

### 7.2  Broadcast helper

```python
def ws_broadcast(payload: str):
    dead = set()
    with _ws_lock:
        for ws in _ws_clients:
            try:
                ws.send(payload)
            except Exception:
                dead.add(ws)
    if dead:
        with _ws_lock:
            _ws_clients -= dead
```

### 7.3  Push thread (1 Hz)

A `threading.Thread(daemon=True)` started at app startup:

```python
def push_thread():
    while True:
        time.sleep(1)
        ws_broadcast(build_status_json())
        maybe_push_fake_log()   # sends a synthetic log entry once every 5 s

threading.Thread(target=push_thread, daemon=True).start()
```

### 7.4  Status JSON builder (mirrors `build_status_json` in firmware)

```python
def build_status_json() -> str:
    with _state_lock:
        s = dict(_state)

    now = time.time() + s['time_offset_s']
    time_str = datetime.utcfromtimestamp(now).strftime('%Y-%m-%dT%H:%M:%S') \
               if now > 1577836800 else ''

    return json.dumps({
        'type': 'status',
        'fg':   { 'temp': s['fg_temp_raw'] / 10.0,
                  'hum':  s['fg_hum_raw']  / 10.0 },
        's200': { 'spd':  s['s200_spd_raw']  / 1000.0,
                  'dir':  s['s200_dir_raw']   / 1000.0 },
        'wifi': { 'mode': s['wifi_mode'],
                  'ip':   s['wifi_ip'],
                  'rssi': s['wifi_rssi'] },
        'time':       time_str,
        'ntp_synced': s['ntp_synced'],
    })
```

### 7.5  Fake log entry (exercises the UI log table)

Pushed once every 5 ticks:

```python
def maybe_push_fake_log():
    if int(time.time()) % 5 != 0:
        return
    entry = json.dumps({
        'type':    'log',
        'ts':      datetime.utcnow().strftime('%H:%M:%S'),
        'dir':     'RX',
        'hex':     '01 03 00 00 00 02 C4 0B',
        'summary': 'FC03 addr=1 reg=0x0000 n=2',
    })
    ws_broadcast(entry)
```

---

## 8  POST `/config/sensor`

Mirrors `handle_post_sensor` in `web_server.cpp` exactly.

**Request body** (JSON):
```json
{ "sensor": "fg6485a"|"s200",
  "addr":   <int 1–247>,         // optional
  "mode":   <int 0|1|2>,         // optional
  "temp":   <float °C>,          // FG6485A only, optional
  "hum":    <float %RH>,         // FG6485A only, optional
  "spd":    <float m/s>,         // S200 only, optional
  "dir":    <float °>,           // S200 only, optional
  "heat":   <float °C>           // S200 only, optional
}
```

**Processing**:
1. Parse JSON; 400 on parse error or missing `sensor` key.
2. For each numeric field present, convert to raw → clamp → update `_state` under `_state_lock` → record whether clamping occurred.
3. Encoding:
   - `temp` → `round(temp * 10)` clamped to `[FG_TEMP_RAW_MIN, FG_TEMP_RAW_MAX]`
   - `hum`  → `round(hum  * 10)` clamped to `[FG_HUM_RAW_MIN, FG_HUM_RAW_MAX]`
   - `spd`  → `round(spd  * 1000)` clamped to `[S200_SPD_RAW_MIN, S200_SPD_RAW_MAX]`
   - `dir`  → `round(dir  * 1000)` clamped to `[S200_DIR_RAW_MIN, S200_DIR_RAW_MAX]`
   - `heat` → `round(heat * 1000)` clamped to `[S200_HEAT_RAW_MIN, S200_HEAT_RAW_MAX]`
   - `addr` clamped to `[1, 247]`

**Response** (JSON): same keys with clamped physical values + `"clamped": bool`.

---

## 9  POST `/config/wifi`

**Request**: `{ "ssid": "...", "pass": "..." }`

**Processing**: Update `_state['wifi_ssid']`; set `wifi_mode` to `"Connecting"`.
Start a `threading.Timer(3, connect_ok)` that after 3 s sets `wifi_mode` to
`"STA"`, sets `wifi_ip` to `"192.168.1.100"`, and sets `wifi_rssi` to −55 to
simulate a successful connect.  This matches the async nature of the real FSM.

**Response**: `{ "ok": true }`

---

## 10  POST `/config/time`

**Request**: `{ "time": "2026-05-04T14:30" }` (datetime-local input format)

**Processing**:
1. Parse with `datetime.strptime(s, '%Y-%m-%dT%H:%M')` (fall back to
   `'%Y-%m-%dT%H:%M:%S'` if a seconds field is present).
2. Compute `offset = parsed_timestamp - time.time()` and store in
   `_state['time_offset_s']`.

This mirrors the `settimeofday()` call in the firmware.

**Response**: `{ "ok": true }`

---

## 11  POST `/config/ntp`

**Request**: `{ "server": "pool.ntp.org" }`

**Processing**: Store in `_state['ntp_server']`.  Optionally simulate a sync:
after 2 s set `_state['ntp_synced'] = True`.

**Response**: `{ "ok": true }`

---

## 12  POST `/replay/upload` and `/replay/control`

Both return:
```
HTTP 501 Not Implemented
{ "error": "not implemented" }
```

Mirrors the Phase 11 stubs in the firmware.

---

## 13  POST `/log/clear`

**Processing**: Broadcast `{ "type": "log_clear" }` to all WebSocket clients.

**Response**: `{ "ok": true }`

---

## 14  `server.py` — Top-level Structure

```python
import json, math, os, threading, time
from datetime import datetime
from flask import Flask, jsonify, request, send_from_directory
from flask_sock import Sock

# ── Paths ─────────────────────────────────────────────────────────────────
BASE   = os.path.dirname(__file__)
DATA   = os.path.join(BASE, '..', 'firmware', 'data')

# ── App ────────────────────────────────────────────────────────────────────
app  = Flask(__name__, static_folder=DATA, static_url_path='')
sock = Sock(app)

# ── Physical range constants ────────────────────────────────────────────────
# (§5)

# ── Shared state ────────────────────────────────────────────────────────────
# (§4)

# ── WebSocket client registry ───────────────────────────────────────────────
# (§7)

# ── Route handlers ──────────────────────────────────────────────────────────
# GET  / and static files          (§6)
# GET  /ws                         (§7.1)
# POST /config/sensor              (§8)
# POST /config/wifi                (§9)
# POST /config/time                (§10)
# POST /config/ntp                 (§11)
# POST /replay/upload              (§12)
# POST /replay/control             (§12)
# POST /log/clear                  (§13)

# ── Push thread startup ──────────────────────────────────────────────────────
# (§7.3)

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, threaded=True)
```

---

## 15  `README.md` Contents

```
# Web Mock Server

Emulates the Atom Lite HTTP/WebSocket interface on a desktop PC.
Serves firmware/data/ directly — no copy needed.

## Setup

    cd webMoc
    pip install -r requirements.txt
    python server.py

Open http://127.0.0.1:5000 in a browser.

## Notes

- State is in-memory only; it resets on restart.
- WiFi connect is simulated with a 3-second delay.
- NTP sync is simulated with a 2-second delay after POST /config/ntp.
- /replay/upload and /replay/control return 501 (Phase 11 stubs).
```

---

## 16  Behavioural Differences from Firmware

| Behaviour | Firmware (Atom Lite) | Mock |
|-----------|---------------------|------|
| State persistence | NVS flash | In-memory only (resets on restart) |
| WiFi connect | Real 802.11 association | Simulated 3 s timer |
| NTP sync | Real SNTP via internet | Simulated 2 s delay |
| Time after `POST /config/time` | `settimeofday()` | Offset added to `time.time()` |
| Log entries | Real Modbus RX/TX | Synthetic fake entries every 5 s |
| Replay / Live mode | Phase 11/10 stubs | Same 501 stub |
| RSSI | Real radio measurement | Fixed −55 dBm after simulated connect |

---

## 17  Implementation Order

1. **Scaffold** — create `webMoc/`, `requirements.txt`, `README.md`, empty `server.py`.
2. **Static file routes** — verify UI loads at `http://127.0.0.1:5000`.
3. **WebSocket skeleton** — `/ws` accepts connections; push thread sends placeholder JSON.
4. **`build_status_json`** — fill in all fields from `_state`; confirm UI status cards update.
5. **`POST /config/sensor`** — implement clamping; verify slider Apply round-trips clamped value.
6. **Remaining POST handlers** — `/config/wifi`, `/config/time`, `/config/ntp`, `/log/clear`.
7. **Fake log push** — verify Modbus log table populates and auto-scrolls.
8. **Simulated WiFi connect** — verify mode/IP/RSSI in status card changes after connect POST.
9. **Smoke test** — step through the full UI manually and confirm no console errors.
