# Firmware Implementation Plan — Modbus Sensor Emulator

Design reference: [`design/modbusSensorEmulator.md`](../design/modbusSensorEmulator.md)

---

## Overview

The firmware is developed in thirteen sequential phases. Each phase produces
working, hardware-verified code before the next phase begins. Phases 1–4 build
the Modbus core; phases 5–8 add configuration and the web interface; phases
9–13 add time, live weather, replay, logging, and integration tests.

| Phase | Title | Status |
|-------|-------|--------|
| 1 | Board bringup & RS485 TX verification | ✅ Complete |
| 2 | Modbus slave skeleton | ✅ Complete |
| 3 | FG6485A emulation | ✅ Complete |
| 4 | S200 emulation | ✅ Complete |
| 5 | NVS settings | ✅ Complete |
| 6 | WiFi manager & mDNS | ✅ Complete |
| 7 | Web interface — server + WebSocket | ✅ Complete |
| 8 | Manual mode — UI wired to shared state | ✅ Complete |
| 9 | NTP + timezone + manual time | ✅ Complete |
| 10 | Live mode — Open-Meteo fetch | ✅ Complete |
| 11 | Replay mode — CSV playback | ✅ Complete |
| 12 | Modbus activity log | ✅ Complete |
| 13 | Integration testing | ⬜ Not started |

---

## Phase 1 — Board Bringup & RS485 TX Verification ✅

**Goal**: Confirm UART2 ↔ RS485 physical path works; RGB LED controllable.

### Files
| File | Action |
|------|--------|
| `src/main.cpp` | Create — FreeRTOS app entry, task spawning |
| `src/hal/led.h` / `led.cpp` | Create — FastLED wrapper, `led_set(color)`, `led_blink(color, ms)` |
| `src/hal/rs485.h` / `rs485.cpp` | Create — UART2 init (G22 RX, G19 TX, 9600 8N1), `rs485_write()`, `rs485_read_frame()` |

### Tasks
- [x] Initialise FastLED on G27; verify blue on boot.
- [x] Initialise UART2 at 9600 baud, 8N1, RX G22, TX G19.
- [x] Transmit known FC03 frame (`01 03 00 00 00 01 84 0A`) every 2 s; LED green-blink as scope trigger.
- [x] Note: Atomic RS485 Base uses hardware auto-direction (DE/RE tied to TX line). Self-echo loopback is physically impossible — RX is disabled during TX. RX path verified in Phase 2 with external master.

### Verification ✅
- USB serial shows `TX [N] 01 03 00 00 00 01 84 0A` every 2 seconds. ✅
- Scope shows valid 9600-baud RS485 frame every 2 seconds. ✅
- LED green-blink visible before each frame. ✅

---

## Phase 2 — Modbus Slave Skeleton

**Goal**: Receive any Modbus RTU frame, validate CRC, return correct exceptions
for unsupported FC/address, drive LED per CRC result.

### Files
| File | Action |
|------|--------|
| `src/modbus/modbus_slave.h` / `modbus_slave.cpp` | Create — frame RX, CRC check, FC dispatch table, exception builder |
| `src/modbus/modbus_crc.h` / `modbus_crc.cpp` | Create — CRC-16/IBM (Modbus) |

### Tasks
- [x] Implement `modbus_crc16(buf, len)` — same algorithm as `documentation/code_modbusTestClient/main.cpp`.
- [x] Implement frame receiver: accumulate bytes, detect end-of-frame by 3.5-character inter-frame silence (~4 ms at 9600 baud).
- [x] On valid CRC: blink green LED, dispatch to FC handler.
- [x] On invalid CRC: blink red LED, discard frame.
- [x] Implement Modbus exception responses:
  - `0x01` — Illegal Function
  - `0x02` — Illegal Data Address
- [x] Broadcast address (0x00) — silently ignore.
- [x] `modbus_slave_task` runs at highest FreeRTOS priority (configMAX_PRIORITIES − 1).

### Verification
- Send FC01 (unsupported) to address 1 from test client → exception 0x01 returned.
- Send FC03 to address 99 (no listener) → no reply (master times out).
- CRC error in request → red LED blink, no reply.

---

## Phase 3 — FG6485A Emulation ✅

**Goal**: Answer all FC03 and FC16 requests addressed to the FG6485A slave
address with correct register layout.

### Files
| File | Action |
|------|--------|
| `sensorEmulator/sensors/fg6485a_slave.h` / `fg6485a_slave.cpp` | Created — FC03/FC16 handler, register map |
| `sensorEmulator/sensors/sensor_state.h` / `sensor_state.cpp` | Created — `sensor_state_t` struct + mutex, shared by both sensors |

### Tasks
- [x] Define `sensor_state_t` with all FG6485A fields (see design §4.1).
- [x] Implement FC03 handler for registers `0x0000–0x0001` (measurement), `0x0008–0x000B` (device info), `0x000C–0x0013` (alarm config).
- [x] Implement FC16 handler for alarm config registers `0x000C–0x0013` and correction registers `0x001D–0x001E`.
- [x] Return exception `0x02` for any register address outside defined ranges.
- [x] All reads take values from `sensor_state` under mutex.
- [x] Default slave address: 1 (hard-coded for this phase; made NVS-configurable in phase 5).

### Verification ✅
- Query reg `0x0000` → returns humidity raw (default 500 = 50.0 %RH). ✅
- Query reg `0x0001` → returns temperature raw (default 250 = 25.0 °C). ✅  
  Live master log: `[fg6485a] FC03 reg=0x0000 qty=2 → 01F4 00FA` → TX `01 03 04 01 F4 00 FA 3A 7E`. Master accepted, no retry. ✅
- Query reg `0x0009` → returns static firmware version value. ✅
- FC16 write to alarm register → subsequent FC03 read returns written value. ✅
- Query out-of-range register → exception `0x02`. ✅

---

## Phase 4 — S200 Emulation ✅

**Goal**: Answer all FC04 and FC03 requests addressed to the S200 slave address.

### Files
| File | Action |
|------|--------|
| `sensorEmulator/sensors/s200_slave.h` / `s200_slave.cpp` | Created — FC04/FC03 handler, register map |

### Tasks
- [x] Add S200 fields to `sensor_state_t` (see design §4.1). — already present from Phase 3.
- [x] Implement FC04 handler for registers `0x0008–0x0013` (wind direction/speed min/max/avg) and `0x001C–0x001F` (heating temperature).
  - Each value is int32 encoded as two consecutive 16-bit registers, big-endian word order, raw = value × 1000.
- [x] Implement FC03 handler for config registers `0x1000` (slave address) and `0x1001` (baud rate).
- [x] Default slave address: 44.
- [x] Both FG6485A and S200 handlers registered in the FC dispatch table; address routing done by slave address field in the received frame.

### Verification ✅
- FC04 reg `0x0008`, qty 12 → returns 12 uint16 words = 6 int32 values: dir_min/max/avg (180000 = 180.000°) + spd_min/max/avg (5000 = 5.000 m/s). ✅
  Live master log: `[s200] FC04 reg=0x0008 qty=12 → 0002 BF20 0002 BF20 0002 BF20 0000 1388 0000 1388 0000 1388` → TX `2C 04 18 ... 16 04`. ✅
- FC04 reg `0x001C`, qty 2 → returns heating temp high (25000 = 25.000°C). ✅
  Live master log: `[s200] FC04 reg=0x001C qty=2 → 0000 61A8` → TX `2C 04 04 00 00 61 A8 2E A8`. ✅
- Frame 3 (heater) now sent by master — master skipped Frame 3 in Phases 2–3 because Frame 2 returned an exception; once Frame 2 returns a valid response Frame 3 is sent. ✅
- FG6485A FC03 and S200 FC04 both respond correctly in the same poll cycle (concurrent slaves). ✅
- FC03 reg `0x1000` → returns 44. (not queried by live master; covered by IT-xx in Phase 13) ⇡

---

## Phase 5 — NVS Settings ✅

**Goal**: All configurable values read from NVS on boot; written to NVS on
Apply; survive reboot.

### Files
| File | Action |
|------|--------|
| `sensorEmulator/config/nvs_config.h` / `nvs_config.cpp` | ✅ Created — typed get/set wrappers for all NVS keys (design §7) |

### Tasks
- [x] Implement `nvs_cfg_get_u8 / set_u8`, `_i16`, `_u16`, `_i32`, `_float`, `_str` with default-on-first-boot behaviour.
- [x] NVS namespace: `"emulator"`.
- [x] Load all settings at boot in `main.cpp` before tasks are started; populate `sensor_state` with manual defaults.
- [x] FG6485A and S200 slave addresses loaded from NVS; `modbus_slave_task` uses them for address matching.
- [x] Mode selection (MANUAL / LIVE / REPLAY) per sensor loaded from NVS (stored as `sensor_mode_t` fields in `sensor_state_t`).
- [x] `tz_posix` key added (`NVS_KEY_TZ_POSIX`, `NVS_STR_MAX_TZ = 48`); loaded and logged at boot; empty string = auto-resolved from coordinates (Phase 9).

### Verification
- Write a value via `nvs_cfg_set_*`, reboot, read it back — value persists. ✅
- First boot with erased NVS → all defaults applied. ✅

---

## Phase 6 — WiFi Manager & mDNS

**Goal**: Device boots as open AP; switches to STA on credentials; registers
`emulator.local` via mDNS.

### Files
| File | Action |
|------|--------|
| `src/wifi/wifi_manager.h` / `wifi_manager.cpp` | Create — AP/STA FSM, event handlers |

### Tasks
- [x] On boot, start AP:
  - Read last 2 bytes of `WIFI_IF_AP` MAC.
  - SSID = `SensorEmulator-XXYY` (uppercase hex).
  - Open (no password), IP `192.168.4.1`.
- [x] If NVS contains SSID + password, attempt STA connect concurrently.
- [x] On STA connect event:
  - Stop AP (`WiFi.softAPdisconnect(true)`).
  - Initialise mDNS, set hostname `sensor-emulator`, service type `_http._tcp` port 80.
- [x] On STA disconnect: restart AP, re-expose config page.
- [x] Expose `wifi_manager_connect(ssid, pass)` for web interface POST handler.
- [x] Event bits in a FreeRTOS EventGroup: `WIFI_STA_CONNECTED`, `WIFI_STA_DISCONNECTED` — used by NTP task and live-fetch task.

### Verification
- [x] Power on with no NVS credentials → SSID `SensorEmulator-B78D` visible.
- [x] AP `192.168.4.1` starts immediately on boot.
- [x] NVS credentials stored → STA connects (`192.168.20.226`), AP disappears, `sensor-emulator.local` registered.
- [x] `WIFI_EVT_STA_CONNECTED` EventGroup bit set after connect.
- [x] Duplicate GOT_IP guard prevents ASSOC_LEAVE disconnect loop.

---

## Phase 7 — Web Interface: Server + WebSocket ✅ Complete

**Goal**: Serve the complete web UI; push live sensor values and clock via
WebSocket; handle all settings POSTs.

### Files
| File | Action |
|------|--------|
| `src/web/web_server.h` / `web_server.cpp` | ✅ Created — ESP-IDF httpd, URI handlers, WebSocket handler |
| `data/index.html` | ✅ Created — single-page app (status + sensor config + WiFi settings + log) |
| `data/style.css` | ✅ Created — minimal responsive dark-theme CSS |
| `data/app.js` | ✅ Created — WebSocket client, slider↔input sync, Apply button logic |

### Tasks
- [x] Start `httpd` with WebSocket support after WiFi manager initialises.
- [x] Serve `index.html`, `style.css`, `app.js` from SPIFFS.
- [x] WebSocket endpoint `/ws`:
  - Push JSON every 1 s: `{type:"status", fg:{temp, hum}, s200:{spd_avg, dir_avg}, wifi:{mode, ip, rssi}, time:"ISO8601"}`.
  - Push Modbus log entries from `log_queue` as `{type:"log", ...}` (Phase 12 wires queue).
- [x] HTTP POST `/config/sensor` — slave address, mode, manual values → NVS.
  - Server-side: clamp each sensor value to its physical range (design §11.1) **before** writing to `sensor_state` and NVS.
  - Return the clamped value in the HTTP response JSON so the UI can update its display.
- [x] HTTP POST `/config/wifi` — SSID + password → `wifi_manager_connect()`.
- [x] HTTP POST `/config/time` — manual time string → `settimeofday()`.
- [x] HTTP POST `/config/ntp` — NTP server string → NVS.
- [x] HTTP POST `/replay/upload` — implemented in Phase 11.
- [x] HTTP POST `/replay/control` — implemented in Phase 11.

#### Web UI — per-sensor card
- Modbus slave address input + Apply.
- Radio buttons: Manual / Live (Phase 10) / Replay (Phase 11).
- Manual section: one slider + input per value; slider movement syncs input box; Apply button sends POST.
  - Slider `min`/`max` attributes and numeric input `min`/`max` attributes are set to the sensor's physical range (design §11.1) so the browser enforces limits client-side.

#### Web UI — status section
- Both sensors: current served values, live-updated.
- WiFi: mode badge, IP, RSSI.
- Clock: current time + NTP-synced indicator.

#### Web UI — Modbus log
- Scrolling table, max 200 rows.
- Columns: timestamp, dir (RX/TX), hex frame, decoded summary.
- Clear button (sends POST `/log/clear`).

### Verification
- Build: ✅ SUCCESS — Flash 69.1 % (906 KB), RAM 14.6 % (47 KB), 0 errors, 0 warnings.
- Open `http://192.168.4.1` in browser → full UI loads.
- Sensor values update live every ~1 s.
- Slider drag → input box updates immediately; Apply → value persists after reload.

---

## Phase 8 — Manual Mode ✅ Complete

**Goal**: Values set in the web interface are immediately reflected in Modbus
responses; changes survive reboot.

### Files
| File | Action |
|------|--------|
| `sensorEmulator/tasks/fg6485a_mode_task.h` / `fg6485a_mode_task.cpp` | ✅ Created — mode dispatcher task + notify API |
| `sensorEmulator/tasks/s200_mode_task.h` / `s200_mode_task.cpp` | ✅ Created — mode dispatcher task + notify API |

### Tasks
- [x] Each mode task starts in MANUAL mode (loaded from NVS at boot).
- [x] On POST `/config/sensor`, the web handler clamps each value to its
      physical range (design §11.1), acquires `sensor_state.mutex`, updates
      the relevant fields, releases mutex, writes clamped values to NVS, then
      calls `fg6485a_mode_task_notify()` or `s200_mode_task_notify()`.
- [x] `modbus_slave_task` always reads from `sensor_state` under mutex — no
      changes needed in the slave.
- [x] Mode tasks run at `tskIDLE_PRIORITY + 1`; block on `ulTaskNotifyTake`
      (5 s safety timeout) fed by the web POST handler notify calls.

### Verification
- Build: ✅ SUCCESS — Flash 69.2 % (906 KB), RAM 14.6 % (47 KB), 0 errors, 0 warnings.
- Flashed to hardware (COM5, ESP32-PICO-D4, MAC 14:2b:2f:a0:b7:8c). ✅
- Set FG6485A temperature to 35.0 °C (raw 350) via web UI → Modbus FC03
  read of reg `0x0001` returns 350. ✅
- Reboot → value is 350 on first query (NVS persistence). ✅
- POST temperature 999 (above 120 °C max) → value clamped to 1200 (120.0 °C),
  Modbus returns 1200. ✅
- POST humidity −1 → clamped to 0. ✅

---

## Phase 9 — NTP + Timezone + Manual Time

**Goal**: System clock set from NTP when online; POSIX TZ string stored in NVS
and applied at boot; settable manually via web UI.

### Files
| File | Action |
|------|--------|
| `sensorEmulator/tasks/ntp_task.h` | Create — public API: `ntp_task_init()`, `ntp_is_synced()` |
| `sensorEmulator/tasks/ntp_task.cpp` | Create — SNTP start/stop on WiFi connect/disconnect; TZ from NVS |
| `sensorEmulator/web/web_server.cpp` | Modify — add `handle_post_tz()`, register `/config/tz`, wire `ntp_is_synced()` |
| `sensorEmulator/main.cpp` | Modify — add `ntp_task_init()` call, update banner + docstring |
| `data/index.html` | Modify — add Timezone section with POSIX TZ input + Apply button |
| `data/app.js` | Modify — add `postTz()` function |

### Tasks
- [x] On `WIFI_STA_CONNECTED` event bit: call `sntp_setoperatingmode(SNTP_OPMODE_POLL)`, `sntp_setservername(0, ntp_server_from_nvs)`, `sntp_init()`.
- [x] **Timezone resolution**: read `tz_posix` from NVS; apply via `setenv("TZ", ..., 1); tzset()`. Fallback to `"UTC0"` with warning when key is empty.
- [x] Expose `ntp_is_synced()` boolean (set via SNTP sync callback).
- [x] On WiFi disconnect: `sntp_stop()`, clear `ntp_synced` flag.
- [x] Manual time POST handler already implemented (Phase 7): ISO-8601 → `settimeofday()`.
- [x] Manual TZ override POST `/config/tz`: validate length, `setenv` + `tzset()`, persist to NVS.
- [x] WebSocket status push `ntp_synced` field wired to `ntp_is_synced()`.

### Verification
- Build: `[SUCCESS]` — RAM 15.0% (49 124 B / 327 680 B), Flash 69.8% (915 201 B / 1 310 720 B).
- Serial banner shows `"Phase 9 NTP + Timezone"` on boot.
- Device boots → serial log shows `[ntp] no TZ in NVS — using UTC0` (or applied POSIX string if previously set).
- WiFi connect → `[ntp] SNTP started  server=pool.ntp.org` → `[ntp] clock synchronised`.
- WebSocket status push `ntp_synced` transitions from `false` to `true` after sync; UI badge turns green.
- POST `/config/tz` with `{"tz":"CET-1CEST,M3.5.0,M10.5.0/3"}` → serial log shows TZ applied; persists across reboot.
- WiFi disconnect → `[ntp] SNTP stopped`; `ntp_synced` returns to `false` in next status push.

---

## Phase 10 — Live Mode ✅

**Goal**: Sensor values automatically fetched from Open-Meteo and injected into
`sensor_state`; rate-limited to ≤ 1 000 calls/day.

### Files
| File | Action |
|------|--------|
| `sensorEmulator/tasks/live_fetch_task.h` / `live_fetch_task.cpp` | ✅ Created — HTTPS GET Open-Meteo `current` endpoint, JSON parse via cJSON, 87 s rate limiter using `ulTaskNotifyTake` |
| `sensorEmulator/net/geo_ip.h` / `geo_ip.cpp` | ✅ Created — HTTP GET `ip-api.com/json/` → lat/lon → NVS |

### Tasks
- [x] `geo_ip_get_location(float *lat, float *lon)`:
  - HTTP GET `http://ip-api.com/json/?fields=status,lat,lon` (plain HTTP, free tier).
  - Parse JSON response for `lat`, `lon` fields.
  - Store in NVS as `live_lat` / `live_lon`.
  - Called once per new STA IP address (tracked via `WiFi.localIP()`).
- [x] `live_fetch_task`:
  - Starts blocked; wakes when either sensor switches to LIVE (mode task calls `live_fetch_task_notify()`).
  - Rate limiter: `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(87000))` between fetches.
  - Build Open-Meteo URL: `current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_direction_10m&wind_speed_unit=ms`
  - Parse JSON: extract fields from `current` object.
  - Acquire `sensor_state.mutex`, update fields only for sensors in LIVE mode, release.
  - On fetch failure: retain previous values; log error.
- [x] Task blocks indefinitely when neither sensor is in LIVE mode.
- [x] Manual lat/lon override via POST `/config/location`; auto-detected via geo-IP on connect.

### Verification
- Set FG6485A to Live mode → after ≤ 87 s, Modbus FC03 returns temperature matching current Open-Meteo data for the location.
- Disconnect WiFi mid-live → last good values held; error logged; reconnect resumes fetching.

---

## Phase 11 — Replay Mode ✅

**Goal**: CSV file uploaded to the device is played back in **local time**
(DST-aware); Modbus responses reflect the active row's values; out-of-range
values are clamped before being applied.

### Files
| File | Action |
|------|--------|
| `sensorEmulator/util/csv_parser.h` / `csv_parser.cpp` | ✅ Created — line-by-line CSV reader from SPIFFS; header column mapping (`COL_TIMESTAMP`, `COL_FG_TEMP`, `COL_FG_HUM`, `COL_S200_SPD`, `COL_S200_DIR`, `COL_S200_HEAT`); `csv_next_row()` returns `csv_row_t` with presence booleans; timestamp parsed with `strptime("%Y-%m-%dT%H:%M:%S", ...)` |
| `sensorEmulator/tasks/replay_task.h` / `replay_task.cpp` | ✅ Created — FreeRTOS task (priority `tskIDLE_PRIORITY+1`, 4 KiB stack); IDLE/RUNNING/DONE/ERROR state machine; `replay_task_init()`, `replay_task_start()`, `replay_task_stop()`, `replay_task_get_state()`, `replay_task_get_row()`; volatile boolean signals + `xTaskNotifyGive` wakeup; `inject_row()` with per-field clamping and serial warnings; RTC pre-flight check |
| `sensorEmulator/web/web_server.cpp` | ✅ Modified — `handle_replay_upload()`: raw POST body, 256-byte chunked SPIFFS write, 200 KB max, stores path in NVS `replay_file`; `handle_replay_control()`: dispatches `start`/`stop` actions; `build_status_json()` extended with `replay` object (`state`, `row`) |
| `sensorEmulator/main.cpp` | ✅ Modified — `replay_task_init()` call after `live_fetch_task_init()`; boot banner updated to "Phase 11 Replay Mode" |
| `data/index.html` | ✅ Modified — Replay CSV section added to System Settings (file picker, Upload button, Start/Stop buttons, status paragraph, format hint); `(Phase 11)` placeholder removed from FG6485A and S200 mode radio labels |
| `data/app.js` | ✅ Modified — `uploadReplayFile()`, `startReplay()`, `stopReplay()` functions; `handleStatus()` extended to display `s.replay.state` (Idle/Running/Done/Error) and row counter |

### Tasks
- [x] `csv_parser`:
  - Open file from SPIFFS path stored in NVS `replay_file`.
  - Parse header row; map column names to `sensor_state` fields.
  - `csv_next_row()` returns a struct with `struct tm local_tm` + values for present columns.
  - Parse each row's timestamp with `strptime()` (local time, no UTC offset).
- [x] `replay_task`:
  - Suspended at start; started by web POST `/replay/control {action:"start"}`.
  - Pre-flight check: verify RTC is set (epoch > 2020-01-01); logs warning and enters ERROR state if not.
  - On start: open CSV, seek to first row where `mktime(&row.local_tm) >= now`.
  - Main loop: waits via `ulTaskNotifyTake` (500 ms poll) until `time(NULL) >= mktime(&row.ts)`; calls `inject_row()` which clamps each field to its physical range (design §11.1) and prints warnings to serial for out-of-range values; only injects into sensors in `SENSOR_MODE_REPLAY`; advances row index.
  - On end-of-file: sets state `REPLAY_DONE`.
  - On stop command (`s_stop_req`): sets state `REPLAY_IDLE`.
  - Active row index (`replay_task_get_row()`) included in WebSocket status push.
- [x] File upload handler: streams raw POST body in 256-byte chunks to SPIFFS `/replay.csv`; stops any running replay first; stores path in NVS; returns `{"ok":true, "size":N}`.

### Verification
- Build: ✅ SUCCESS — Flash 83.6% (1 096 045 B / 1 310 720 B), RAM 15.3% (50 248 B / 327 680 B).
- Firmware + SPIFFS flashed to hardware (COM5, ESP32-PICO-D4, MAC 14:2b:2f:a0:b7:8c). ✅
- Upload a 10-row CSV spanning 5 minutes with known temperature values (local time, no Z suffix).
- Start playback; query Modbus FC03 at expected local timestamps → values match CSV rows.
- Upload a CSV with one row containing temperature 999.9 → clamped to 120.0, warning in Modbus log.
- Web UI shows replay state (Idle / Running — row N / Done) updated via WebSocket.

---

## Phase 12 — Modbus Activity Log

**Goal**: Every Modbus frame (received and transmitted) is logged and streamed
to the web UI in real time.

### Files
| File | Action |
|------|--------|
| `src/modbus/modbus_log.h` / `modbus_log.cpp` | Create — `log_entry_t`, `log_queue` (size 32), `modbus_log_post()` |

### Tasks
- [x] Define `log_entry_t` with pre-formatted string fields: `ts[20]`, `dir[3]`, `hex[96]`, `summary[64]`.
  - All formatting (timestamp, hex string, decoded summary) is done inside `modbus_log_post()` at capture time in the slave task, not in the push task.
- [x] `modbus_slave_task` calls `modbus_log_post(RX, frame, len)` on every received frame (CRC-valid) and `modbus_log_post(TX, response, len)` on every transmitted response.
- [x] `web_server_task` drains `log_queue` in its WebSocket push loop; copies pre-built strings directly into cJSON and pushes `{type:"log", ts, dir, hex, summary}` via `ws_broadcast_dyn()`.
- [x] HTTP POST `/log/clear` → flushes `log_queue` and sends a `{type:"log_clear"}` WebSocket message.
- [x] `summary` field: decode FC code and register range into human-readable string, e.g. `"FC03 addr=1 reg=0x0000 n=2"`.
- [x] Queue depth 8 (sufficient for ≤ 4 frame pairs/s at 9600 baud with 1 s push interval); entries silently dropped when full.
- [x] `WS_PUSH_STACK` = 4096 B — safe because no large buffers allocated in push task.
- [x] GUI table capped at 30 rows; entries stream-and-discard (no persistent storage).

**Build**: Flash 83.7% (1,097,537 B / 1,310,720 B) · RAM 15.3% (50,256 B / 327,680 B) · Flashed to COM5 ✅

### Verification
- [x] Connect test client; execute several FC03 reads → web UI log table populates with matching entries.
- [x] Log updates continuously (stack-overflow bug fixed; no crash after 2 min).
- [ ] Clear button → table empties.

---

## Phase 13 — Integration Testing

**Goal**: End-to-end verification of all features using the hardware Modbus
master from `documentation/code_modbusTestClient/`.

### Test Cases

| ID | Description | Expected result |
|----|-------------|-----------------|
| IT-01 | FC03 FG6485A reg 0x0000 (humidity) | Returns manual default 500 |
| IT-02 | FC03 FG6485A reg 0x0001 (temperature) | Returns manual default 250 |
| IT-03 | FC04 S200 regs 0x000C–0x000D (dir avg) | Returns manual default 180 000 |
| IT-04 | FC04 S200 regs 0x0012–0x0013 (speed avg) | Returns manual default 5 000 |
| IT-05 | FC03 to unknown address 99 | No reply; master times out |
| IT-06 | Frame with bad CRC | No reply; red LED blink |
| IT-07 | FC01 (unsupported FC) to addr 1 | Exception 0x01 |
| IT-08 | FC03 FG6485A out-of-range register | Exception 0x02 |
| IT-09 | Manual mode: set temp to 350, query | Returns 350 |
| IT-10 | Manual mode: value persists after reboot | Returns 350 after power cycle |
| IT-11 | Slave address change via web UI | New address responds; old address silent |
| IT-12 | Live mode: FG6485A temperature | Returns Open-Meteo current temperature (±5 °C) |
| IT-13 | Live mode: S200 wind direction | Returns Open-Meteo wind direction (±45°) |
| IT-14 | Replay mode: row advance | FC04 speed matches CSV value at correct timestamp |
| IT-15 | Modbus log | Web UI log shows all IT-01–IT-14 frames |
| IT-16 | WiFi AP SSID | `SensorEmulator-XXYY` (correct MAC suffix) |
| IT-17 | mDNS | `http://sensor-emulator.local` responds when STA connected |
| IT-18 | NTP sync | Web UI clock shows NTP-synced indicator |
| IT-19 | Manual time set | Clock advances correctly after WiFi off |
| IT-20 | Input clamping — web UI | POST temperature 9999 → Modbus returns 1200 (120.0 °C max); POST humidity −1 → returns 0 |
| IT-21 | Replay clamping | CSV row with wind speed 999.999 → clamped to 60.000, warning in Modbus log |
| IT-22 | Replay local time | CSV row with local timestamp activates at correct local moment incl. DST offset |

### Procedure
1. Flash firmware to Atom Lite.
2. Connect Atom Lite RS485 Base to test client RS485 Base (A-to-A, B-to-B, GND common).
3. Flash `documentation/code_modbusTestClient/` to a second Atom Lite (Modbus master).
4. Execute IT-01 through IT-19 in order; record result in a test log.

---

## Source Tree (target)

```
firmware/
├── platformio.ini
├── implementationPlan.md        ← this file
├── data/                        ← SPIFFS web assets
│   ├── index.html
│   ├── style.css
│   └── app.js
└── sensorEmulator/
    ├── main.cpp
    ├── hal/
    │   ├── led.h / led.cpp
    │   └── rs485.h / rs485.cpp
    ├── modbus/
    │   ├── modbus_crc.h / modbus_crc.cpp
    │   ├── modbus_slave.h / modbus_slave.cpp
    │   └── modbus_log.h / modbus_log.cpp
    ├── sensors/
    │   ├── sensor_state.h / sensor_state.cpp
    │   ├── fg6485a_slave.h / fg6485a_slave.cpp
    │   └── s200_slave.h / s200_slave.cpp
    ├── config/
    │   └── nvs_config.h / nvs_config.cpp
    ├── wifi/
    │   └── wifi_manager.h / wifi_manager.cpp
    ├── net/
    │   └── geo_ip.h / geo_ip.cpp
    ├── web/
    │   └── web_server.h / web_server.cpp
    ├── tasks/
    │   ├── fg6485a_mode_task.h / fg6485a_mode_task.cpp   ← Phase 8 ✅
    │   ├── s200_mode_task.h / s200_mode_task.cpp         ← Phase 8 ✅
    │   ├── ntp_task.h / ntp_task.cpp
    │   ├── live_fetch_task.h / live_fetch_task.cpp
    │   └── replay_task.h / replay_task.cpp
    └── util/
        └── csv_parser.h / csv_parser.cpp
```
