# Web Mock Server

Emulates the Atom Lite HTTP/WebSocket interface on a desktop PC for developing
and testing the web UI without flashing any firmware.

Web assets are served directly from `firmware/data/` — no copy needed.

## Prerequisites

- **Python 3.10 or newer** — verify with `python --version`
- **pip** — included with all modern Python distributions
- The packages listed in `requirements.txt`:

| Package | Version | Purpose |
|---------|---------|---------|
| `flask` | ≥ 3.0 | HTTP server and routing |
| `flask-sock` | ≥ 0.7 | WebSocket support (`simple-websocket` backend) |

Install them once before the first run:

```
pip install -r requirements.txt
```

No other tools, compilers, or hardware are needed.

## Setup and run

```
cd webMoc
pip install -r requirements.txt
python server.py
```

Open <http://127.0.0.1:5000> in a browser.

## Endpoints

| Method | Path | Behaviour |
|--------|------|-----------|
| GET | `/` `/index.html` `/style.css` `/app.js` | Serves `firmware/data/` directly |
| GET | `/ws` | WebSocket; pushes `status` JSON every 1 s |
| POST | `/config/sensor` | Clamps values, updates in-memory state, returns clamped values |
| POST | `/config/wifi` | Simulates a 3-second STA connect |
| POST | `/config/time` | Stores a clock offset (mirrors `settimeofday`) |
| POST | `/config/ntp` | Saves server name; simulates NTP sync after 2 s |
| POST | `/replay/upload` | 501 Not Implemented (Phase 11 stub) |
| POST | `/replay/control` | 501 Not Implemented (Phase 11 stub) |
| POST | `/log/clear` | Broadcasts `{type:"log_clear"}` to all WebSocket clients |

## Notes

- State is **in-memory only** — it resets on server restart.
- Fake Modbus log entries are pushed via WebSocket every 5 s to exercise
  the log table in the UI.
- WiFi connect is simulated with a 3-second delay; mode changes AP → Connecting → STA.
- NTP sync is simulated with a 2-second delay after `POST /config/ntp`.
- RSSI is fixed at −55 dBm after the simulated STA connect.

## Differences from firmware

| | Firmware | Mock |
|-|----------|------|
| State persistence | NVS flash | In-memory (lost on restart) |
| WiFi connect | Real 802.11 | 3 s timer |
| NTP sync | Real SNTP | 2 s timer |
| Clock | `settimeofday()` | `time.time()` offset |
| Log entries | Real Modbus RX/TX | Synthetic frames every 5 s |
| RSSI | Radio measurement | Fixed −55 dBm |
