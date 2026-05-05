"""
server.py — Web Mock for the Modbus Sensor Emulator (Phase 12)

Emulates the HTTP + WebSocket interface of firmware/sensorEmulator/web/web_server.cpp
on a desktop PC. Serves firmware/data/ assets directly and simulates all endpoints
with the same request/response contract as the Atom Lite firmware.

Endpoints:
    GET  /                   — serves index.html from firmware/data/
    GET  /ws                 — WebSocket; pushes status JSON (1 Hz) + fake log entries
    POST /config/sensor      — addr/mode/manual values for fg6485a or s200
    POST /config/wifi        — SSID/password; simulates 3 s connect
    POST /config/time        — set clock offset (ISO-8601 datetime)
    POST /config/tz          — POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3")
    POST /config/ntp         — NTP server; simulates 2 s sync
    POST /config/location    — lat/lon for Live-mode weather fetch
    POST /replay/upload      — CSV body; stores in memory
    POST /replay/control     — {"action":"start"|"stop"}
    POST /log/clear          — broadcasts {"type":"log_clear"} WS message

Usage:
    cd webMock
    pip install -r requirements.txt
    python server.py
    # Open http://127.0.0.1:5000
"""

import json
import os
import threading
import time
from datetime import datetime, timezone

from flask import Flask, jsonify, request, send_from_directory
from flask_sock import Sock

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.normpath(os.path.join(BASE_DIR, '..', 'firmware', 'data'))

# ---------------------------------------------------------------------------
# Flask app
# ---------------------------------------------------------------------------

app  = Flask(__name__, static_folder=DATA_DIR, static_url_path='')
sock = Sock(app)

# ---------------------------------------------------------------------------
# Physical range constants — mirrors sensor_state.h
# ---------------------------------------------------------------------------

FG_TEMP_RAW_MIN,   FG_TEMP_RAW_MAX  = -400,    1200   # °C × 10
FG_HUM_RAW_MIN,    FG_HUM_RAW_MAX   =    0,     999   # %RH × 10
S200_SPD_RAW_MIN,  S200_SPD_RAW_MAX =    0,   60000   # m/s × 1000
S200_DIR_RAW_MIN,  S200_DIR_RAW_MAX =    0,  360000   # ° × 1000
S200_HEAT_RAW_MIN, S200_HEAT_RAW_MAX = -40000, 85000  # °C × 1000


def clamp(v, lo, hi):
    """Mirrors clamp_int() in web_server.cpp."""
    return max(lo, min(hi, v))

# ---------------------------------------------------------------------------
# Simulated device state — mirrors g_sensor_state + NVS
# ---------------------------------------------------------------------------

_state = {
    # FG6485A
    'fg_addr':       1,
    'fg_mode':       0,        # 0=MANUAL 1=LIVE 2=REPLAY
    'fg_temp_raw':   250,      # int16:  raw = °C × 10
    'fg_hum_raw':    500,      # uint16: raw = %RH × 10
    # S200
    's200_addr':     44,
    's200_mode':     0,
    's200_spd_raw':  5000,     # int32: raw = m/s × 1000
    's200_dir_raw':  180000,   # int32: raw = ° × 1000
    's200_heat_raw': 25000,    # int32: raw = °C × 1000
    # WiFi
    'wifi_mode':     'AP',     # AP | Connecting | STA
    'wifi_ip':       '192.168.4.1',
    'wifi_rssi':     0,
    'wifi_ssid':     '',
    # Clock
    'time_offset_s': 0,        # added to time.time() to simulate settimeofday()
    'ntp_synced':    False,
    'ntp_server':    'pool.ntp.org',
    'tz_posix':      'UTC0',
    # Live-fetch location
    'live_lat':      52.37,
    'live_lon':       4.90,
    'live_fetch_age': 0,
    'live_fetch_ok':  True,
    # Replay
    'replay_state':  'idle',   # idle | running | done | error
    'replay_row':    0,
    'replay_csv':    None,     # raw CSV bytes stored in memory
}
_state_lock = threading.Lock()

# ---------------------------------------------------------------------------
# WebSocket client registry
# ---------------------------------------------------------------------------

_ws_clients: set = set()
_ws_lock = threading.Lock()

# Sequence counter for fake log entries.
_log_tick = 0
# Alternating fake frames to make the log more varied.
_FAKE_FRAMES = [
    ('01 03 00 00 00 02 C4 0B', 'RX', 'FC03 addr=1 reg=0x0000 n=2'),
    ('01 03 04 01 F4 00 FA 3A 7E', 'TX', 'FC03 resp addr=1 reg=0x0000 n=2'),
    ('2C 04 00 08 00 0C 50 19', 'RX', 'FC04 addr=44 reg=0x0008 n=12'),
    ('2C 04 18 00 02 BF 20 00 02 BF 20 00 02 BF 20 00 00 13 88 00 00 13 88 00 00 13 88 16 04', 'TX', 'FC04 resp addr=44 reg=0x0008 n=12'),
]


def ws_broadcast(payload: str) -> None:
    """Send a text frame to all connected WebSocket clients; remove dead ones."""
    dead = set()
    with _ws_lock:
        clients = set(_ws_clients)
    for ws in clients:
        try:
            ws.send(payload)
        except Exception:
            dead.add(ws)
    if dead:
        with _ws_lock:
            _ws_clients.difference_update(dead)


def build_status_json() -> str:
    """Build a status push payload identical to firmware's build_status_json()."""
    with _state_lock:
        s = dict(_state)

    now = time.time() + s['time_offset_s']
    # Only format if clock appears set (> 2020-01-01), same guard as firmware.
    time_str = (
        datetime.fromtimestamp(now, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%S')
        if now > 1_577_836_800
        else ''
    )

    return json.dumps({
        'type': 'status',
        'fg': {
            'temp': s['fg_temp_raw'] / 10.0,
            'hum':  s['fg_hum_raw']  / 10.0,
            'mode': s['fg_mode'],
            'addr': s['fg_addr'],
        },
        's200': {
            'spd':  s['s200_spd_raw']  / 1000.0,
            'dir':  s['s200_dir_raw']  / 1000.0,
            'heat': s['s200_heat_raw'] / 1000.0,
            'mode': s['s200_mode'],
            'addr': s['s200_addr'],
        },
        'wifi': {
            'mode': s['wifi_mode'],
            'ip':   s['wifi_ip'],
            'rssi': s['wifi_rssi'],
            'ssid': s['wifi_ssid'],
        },
        'time':           time_str,
        'ntp_synced':     s['ntp_synced'],
        'live': {
            'lat': s['live_lat'],
            'lon': s['live_lon'],
        },
        'live_fetch_age': s['live_fetch_age'],
        'live_fetch_ok':  s['live_fetch_ok'],
        'replay': {
            'state': s['replay_state'],
            'row':   s['replay_row'],
        },
    })


def _push_thread() -> None:
    """Background thread: 1 Hz status push + synthetic Modbus log entries."""
    global _log_tick
    while True:
        time.sleep(1)
        ws_broadcast(build_status_json())
        # Push a fake log entry every 5 ticks.
        if _log_tick % 5 == 0:
            frame_hex, direction, summary = _FAKE_FRAMES[(_log_tick // 5) % len(_FAKE_FRAMES)]
            ws_broadcast(json.dumps({
                'type':    'log',
                'ts':      datetime.now(tz=timezone.utc).strftime('%Y-%m-%d %H:%M:%S'),
                'dir':     direction,
                'hex':     frame_hex,
                'summary': summary,
            }))
        _log_tick += 1


# ---------------------------------------------------------------------------
# Static file routes
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# WebSocket endpoint
# ---------------------------------------------------------------------------

@sock.route('/ws')
def ws_handler(ws):
    with _ws_lock:
        _ws_clients.add(ws)
    try:
        while True:
            # Block with a timeout so we detect disconnects promptly.
            msg = ws.receive(timeout=5)
            if msg is None:
                break
            # Incoming frames are discarded (push-only, matching Phase 7 firmware).
    except Exception:
        pass
    finally:
        with _ws_lock:
            _ws_clients.discard(ws)


# ---------------------------------------------------------------------------
# POST /config/sensor
# Mirrors handle_post_sensor() in web_server.cpp exactly.
# ---------------------------------------------------------------------------

@app.route('/config/sensor', methods=['POST'])
def config_sensor():
    body = request.get_json(silent=True)
    if not body or 'sensor' not in body:
        return jsonify({'error': 'Missing sensor'}), 400

    sensor  = body['sensor']
    clamped = False
    resp    = {'sensor': sensor}

    if sensor == 'fg6485a':
        with _state_lock:
            if 'addr' in body:
                raw = int(body['addr'])
                val = clamp(raw, 1, 247)
                _state['fg_addr'] = val
                resp['addr'] = val

            if 'mode' in body:
                _state['fg_mode'] = clamp(int(body['mode']), 0, 2)

            if 'temp' in body:
                raw = round(float(body['temp']) * 10)
                cr  = clamp(raw, FG_TEMP_RAW_MIN, FG_TEMP_RAW_MAX)
                if cr != raw:
                    clamped = True
                _state['fg_temp_raw'] = cr
                resp['temp'] = cr / 10.0

            if 'hum' in body:
                raw = round(float(body['hum']) * 10)
                cr  = clamp(raw, FG_HUM_RAW_MIN, FG_HUM_RAW_MAX)
                if cr != raw:
                    clamped = True
                _state['fg_hum_raw'] = cr
                resp['hum'] = cr / 10.0

    elif sensor == 's200':
        with _state_lock:
            if 'addr' in body:
                val = clamp(int(body['addr']), 1, 247)
                _state['s200_addr'] = val
                resp['addr'] = val

            if 'mode' in body:
                _state['s200_mode'] = clamp(int(body['mode']), 0, 2)

            if 'spd' in body:
                raw = round(float(body['spd']) * 1000)
                cr  = clamp(raw, S200_SPD_RAW_MIN, S200_SPD_RAW_MAX)
                if cr != raw:
                    clamped = True
                _state['s200_spd_raw'] = cr
                resp['spd'] = cr / 1000.0

            if 'dir' in body:
                raw = round(float(body['dir']) * 1000)
                cr  = clamp(raw, S200_DIR_RAW_MIN, S200_DIR_RAW_MAX)
                if cr != raw:
                    clamped = True
                _state['s200_dir_raw'] = cr
                resp['dir'] = cr / 1000.0

            if 'heat' in body:
                raw = round(float(body['heat']) * 1000)
                cr  = clamp(raw, S200_HEAT_RAW_MIN, S200_HEAT_RAW_MAX)
                if cr != raw:
                    clamped = True
                _state['s200_heat_raw'] = cr
                resp['heat'] = cr / 1000.0

    else:
        return jsonify({'error': 'Unknown sensor'}), 400

    resp['clamped'] = clamped
    return jsonify(resp)


# ---------------------------------------------------------------------------
# POST /config/wifi
# Mirrors handle_post_wifi(); simulates a 3-second STA connect.
# ---------------------------------------------------------------------------

@app.route('/config/wifi', methods=['POST'])
def config_wifi():
    body = request.get_json(silent=True) or {}
    ssid = body.get('ssid', '')
    with _state_lock:
        _state['wifi_ssid'] = ssid
        _state['wifi_mode'] = 'Connecting'
        _state['wifi_ip']   = ''
        _state['wifi_rssi'] = 0

    def _connect():
        time.sleep(3)
        with _state_lock:
            _state['wifi_mode'] = 'STA'
            _state['wifi_ip']   = '192.168.1.100'
            _state['wifi_rssi'] = -55

    threading.Timer(3, _connect).start()
    return jsonify({'ok': True})


# ---------------------------------------------------------------------------
# POST /config/time
# Mirrors handle_post_time(); stores an offset against time.time().
# ---------------------------------------------------------------------------

@app.route('/config/time', methods=['POST'])
def config_time():
    body = request.get_json(silent=True) or {}
    ts   = body.get('time', '')
    if ts:
        # Try with seconds first, then without (datetime-local omits seconds).
        for fmt in ('%Y-%m-%dT%H:%M:%S', '%Y-%m-%dT%H:%M'):
            try:
                dt = datetime.strptime(ts, fmt)
                # Treat parsed time as local time (same as firmware mktime()).
                target = dt.replace(tzinfo=None).timestamp()
                with _state_lock:
                    _state['time_offset_s'] = target - time.time()
                    _state['ntp_synced']    = False
                break
            except ValueError:
                continue
    return jsonify({'ok': True})


# ---------------------------------------------------------------------------
# POST /config/ntp
# Mirrors handle_post_ntp(); simulates sync after 2 seconds.
# ---------------------------------------------------------------------------

@app.route('/config/ntp', methods=['POST'])
def config_ntp():
    body = request.get_json(silent=True) or {}
    server = body.get('server', 'pool.ntp.org')
    with _state_lock:
        _state['ntp_server'] = server

    def _sync():
        time.sleep(2)
        with _state_lock:
            _state['ntp_synced'] = True

    threading.Timer(2, _sync).start()
    return jsonify({'ok': True})


# ---------------------------------------------------------------------------
# POST /replay/upload
# POST /replay/control
# ---------------------------------------------------------------------------

@app.route('/replay/upload', methods=['POST'])
def replay_upload():
    data = request.get_data()
    if not data:
        return jsonify({'error': 'Empty body'}), 400
    with _state_lock:
        _state['replay_csv']   = data
        _state['replay_state'] = 'idle'
        _state['replay_row']   = 0
    return jsonify({'ok': True, 'size': len(data)})


@app.route('/replay/control', methods=['POST'])
def replay_control():
    body   = request.get_json(silent=True) or {}
    action = body.get('action', '')
    with _state_lock:
        if action == 'start' and _state['replay_csv']:
            _state['replay_state'] = 'running'
            _state['replay_row']   = 0
        elif action == 'stop':
            _state['replay_state'] = 'idle'
        state = _state['replay_state']
    return jsonify({'ok': True, 'state': state})


# ---------------------------------------------------------------------------
# POST /config/tz
# Mirrors handle_post_tz(); stores POSIX TZ string.
# ---------------------------------------------------------------------------

@app.route('/config/tz', methods=['POST'])
def config_tz():
    body    = request.get_json(silent=True) or {}
    tz_str  = body.get('tz', '')
    applied = tz_str if tz_str else 'UTC0'
    with _state_lock:
        _state['tz_posix'] = applied
    return jsonify({'ok': True, 'tz': applied})


# ---------------------------------------------------------------------------
# POST /config/location
# Mirrors handle_post_location(); stores lat/lon for Live-mode fetch.
# ---------------------------------------------------------------------------

@app.route('/config/location', methods=['POST'])
def config_location():
    body = request.get_json(silent=True) or {}
    if 'lat' not in body or 'lon' not in body:
        return jsonify({'error': 'Missing lat/lon'}), 400
    lat = max(-90.0,  min(90.0,  float(body['lat'])))
    lon = max(-180.0, min(180.0, float(body['lon'])))
    with _state_lock:
        _state['live_lat'] = lat
        _state['live_lon'] = lon
    return jsonify({'ok': True, 'lat': lat, 'lon': lon})


# ---------------------------------------------------------------------------
# POST /log/clear
# Mirrors handle_post_log_clear(); broadcasts log_clear WS message.
# ---------------------------------------------------------------------------

@app.route('/log/clear', methods=['POST'])
def log_clear():
    ws_broadcast(json.dumps({'type': 'log_clear'}))
    return jsonify({'ok': True})


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    threading.Thread(target=_push_thread, daemon=True).start()
    print(f'[mock] Serving web assets from: {DATA_DIR}')
    print('[mock] Open http://127.0.0.1:5000')
    app.run(host='0.0.0.0', port=5000, threaded=True)
