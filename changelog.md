# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [0.7.0] — 2026-05-04  Phase 7 — Web Interface: Server + WebSocket

### Added — firmware (`firmware/sensorEmulator/`)
- `web/web_server.h` / `web_server.cpp` — ESP-IDF httpd server on port 80:
  - Serves `index.html`, `style.css`, `app.js` from SPIFFS (chunked transfer).
  - WebSocket endpoint `/ws` — 1 Hz status push via `httpd_queue_work` +
    `httpd_ws_send_frame_async` pattern.  Status JSON: `{type:"status",
    fg:{temp, hum}, s200:{spd, dir}, wifi:{mode, ip, rssi}, time, ntp_synced}`.
  - `POST /config/sensor` — clamps physical values to design §11 ranges,
    writes `g_sensor_state` under mutex + NVS, returns clamped values +
    `"clamped":bool`.
  - `POST /config/wifi` — calls `wifi_manager_connect(ssid, pass)`.
  - `POST /config/time` — `strptime` → `mktime` → `settimeofday`.
  - `POST /config/ntp` — saves server string to NVS.
  - `POST /replay/upload`, `POST /replay/control` — 501 stubs for Phase 11.
  - `POST /log/clear` — broadcasts `{type:"log_clear"}` WebSocket frame.
- `data/index.html` — dark-theme single-page app: status grid (FG6485A, S200,
  WiFi, clock), FG6485A config card (addr + Manual/Live/Replay + sliders),
  S200 config card, WiFi/NTP/time settings, Modbus log table.
- `data/style.css` — CSS variables dark theme, `.badge`, `.grid4`, `.slider-row`,
  `.card`, `#log-wrap` scrollable table.
- `data/app.js` — `wsConnect()` (3 s reconnect), `handleStatus()`, `linkSlider()`
  for all five slider pairs, `post()` fetch helper, `postFgManual/Addr`,
  `postS200Manual/Addr`, `postWifi/Ntp/Time`, `appendLog/clearLogTable/postLogClear`,
  XSS-safe `esc()` helper.

### Changed — firmware
- `platformio.ini` — added `board_build.filesystem = spiffs` to `[env:sensorEmulator]`.
- `sensorEmulator/main.cpp` — Phase 7 banner; `#include "web/web_server.h"`;
  `web_server_init()` called after `wifi_manager_init()`.

### Verified
- Build: SUCCESS, 0 errors, 0 warnings (Flash 69.1 % — 906 KB, RAM 14.6 % — 47 KB).
- Flash procedure: `pio run -t uploadfs -e sensorEmulator` (SPIFFS), then
  `pio run -t upload -e sensorEmulator` (firmware).

---

## [0.6.1] — 2026-05-04  Design & sensor-state: timezone, local-time replay, input clamping

### Changed — design (`design/modbusSensorEmulator.md`)
- **§8 Time Management** split into three subsections:
  - §8.1 Clock source — NTP delivers UTC; manual fallback via `settimeofday()`.
  - §8.2 Local time determination — timezone resolved from `live_lat`/`live_lon` coordinates to a POSIX TZ string (IANA rules, DST-aware); applied via `setenv("TZ",…)/tzset()`; UTC fallback on first boot; manual POSIX TZ override stored as NVS key `tz_posix`.
  - §8.3 Display — WebSocket push shows DST-corrected local time + NTP-synced indicator.
- **§2.3 Replay** — timestamps in CSV are local time (no UTC offset); comparison uses `localtime_r()` + `mktime()` for DST correctness; pre-flight warning if clock/timezone not ready; out-of-range CSV values clamped before applying.
- **§4 FreeRTOS diagram** — `replay_task` description updated to "local clock time (DST-aware)".
- **§6 Sensor Configuration** — Manual slider/input `min`/`max` attributes constrained to physical sensor ranges; `/config/sensor` POST handler clamps server-side and returns clamped value.
- **§7 NVS settings table** — added `tz_posix` key (str, default "" = auto-resolved).
- **§10 CSV Replay File Format** — example timestamps no longer carry `Z` suffix; new bullet documents `strptime()`/`localtime_r()` comparison, DST correctness, and pre-flight clock/timezone check.
- **New §11 Input Validation & Clamping** — physical range table for all five sensor fields (FG6485A temp/humidity, S200 wind speed/direction/heating temp) with raw encoding; clamping rules for web UI and CSV replay; warning logged for clamped replay rows.
- Old §11 Sequence of Development → §12; old §12 Reference Documentation → §13.

### Changed — implementation plan (`firmware/implementationPlan.md`)
- Phase 9 title updated to "NTP + Timezone + Manual Time"; tasks rewritten to cover POSIX TZ resolution from coordinates, manual TZ override POST, and local-time WebSocket push.
- Phase 11 Replay rewritten: `csv_parser` uses `strptime()`/`struct tm`; `replay_task` uses `mktime()`/`localtime_r()` for DST-aware activation; clamping with `log_queue` warning.
- Phase 7 Web UI task: slider `min`/`max` attributes set to sensor physical range.
- Phase 8 Manual mode: `/config/sensor` POST handler clamps values before `sensor_state` write and NVS commit; verification cases for over/under-range inputs added.
- Phase 5 task: `tz_posix` NVS key loaded and logged at boot.
- IT-20 / IT-21 / IT-22 integration test cases added (UI clamping, replay clamping, local-time replay).

### Changed — firmware (`firmware/sensorEmulator/`)
- `config/nvs_config.h` — added `NVS_KEY_TZ_POSIX` (`"tz_posix"`) and `NVS_STR_MAX_TZ` (48 bytes).
- `config/nvs_config.cpp` — `nvs_cfg_load_all()` reads and logs `tz_posix` at boot.
- `sensors/sensor_state.h` — added physical range constants for all clamping consumers (Phases 8, 10, 11):
  - `FG6485A_TEMP_RAW_MIN/MAX` (−400 / 1200), `FG6485A_HUM_RAW_MIN/MAX` (0 / 999)
  - `S200_SPD_RAW_MIN/MAX` (0 / 60 000), `S200_DIR_RAW_MIN/MAX` (0 / 360 000), `S200_HEAT_RAW_MIN/MAX` (−40 000 / 85 000)

### Verified
- Build: SUCCESS, 0 warnings/errors (EXIT:0, Flash 59.9 %, RAM 14.1 %).

---

## [0.6.0] — 2026-05-06  Phase 6: WiFi Manager & mDNS

### Added
- `firmware/sensorEmulator/wifi/wifi_manager.h` / `wifi_manager.cpp` — WiFi AP/STA FSM:
  - On boot: open AP `SensorEmulator-XXYY` (last 2 MAC bytes, IP `192.168.4.1`).
  - If NVS contains `wifi_ssid` / `wifi_pass`: attempts STA connect concurrently.
  - On `STA_GOT_IP`: disables AP, starts mDNS hostname `emulator` (`http://emulator.local`), sets `WIFI_EVT_STA_CONNECTED` EventGroup bit.
  - On `STA_DISCONNECTED`: stops mDNS, restarts AP, auto-retries stored credentials.
  - `wifi_manager_connect(ssid, pass)` — queued connect request for web interface (Phase 7).
  - `wifi_manager_get_event_group()`, `wifi_manager_get_state()`, `wifi_manager_get_sta_ip()`, `wifi_manager_get_ap_ssid()` — accessors for other tasks.
- All WiFi/mDNS calls confined to `wifi_manager_task` (priority 2, stack 8 KiB); event callback only posts FreeRTOS task-notification bits.
- Duplicate `STA_GOT_IP` guard (`s_state != WIFI_STATE_STA`) prevents ASSOC_LEAVE disconnect loop caused by ESP-IDF firing the event twice during mode transitions.

### Changed
- `firmware/sensorEmulator/main.cpp` — Phase 6 banner; `wifi_manager_init()` called after Modbus slave task starts.

---

## [Unreleased]

### Added
- Initial design plan (`design/modbusSensorEmulator.md`) covering:
  - FreeRTOS task architecture (modbus slave, per-sensor mode tasks, live-fetch, replay, NTP, web server, WiFi manager)
  - Modbus register maps for FG6485A (FC03) and S200 (FC04)
  - Three sensor modes per sensor: manual, live (Open-Meteo API), replay (timestamped CSV)
  - Web interface layout: status, per-sensor config cards, WiFi settings, Modbus activity log
  - NVS settings key table
  - WiFi AP/STA switching with mDNS (`emulator.local`)
  - NTP + manual time fallback
  - IP-geolocation for automatic location detection (live mode)
  - CSV replay file format
  - RGB LED behaviour (blue idle / green blink / red blink / red solid)
  - Development sequence

---

## [0.5.0] — 2026-05-04  Phase 5: NVS Settings

### Added
- `firmware/sensorEmulator/config/nvs_config.h` / `nvs_config.cpp` — persistent settings layer using ESP-IDF NVS under namespace `"emulator"`:
  - Typed getter/setter API: `nvs_cfg_get_u8/i16/u16/i32/float/str` and matching `nvs_cfg_set_*`.  Getters return a caller-supplied default when the key is absent (first boot) or on error.
  - `float` stored as 4-byte blob (`nvs_get_blob`/`nvs_set_blob`) since ESP-IDF NVS has no native float type.
  - Setters call `nvs_commit()` immediately so no buffered writes are lost on unexpected resets.
  - `nvs_cfg_init()` handles corrupt/version-changed NVS partitions by erasing and re-initialising.
  - `nvs_cfg_load_all(fg_addr_out, s200_addr_out)` reads all design §7 keys into `g_sensor_state` and returns the configured slave addresses.
  - Key constants for all 14 design §7 NVS keys defined as `NVS_KEY_*` macros; all ≤ 15 characters (ESP-IDF limit).
- `sensor_mode_t` enum (`SENSOR_MODE_MANUAL=0`, `SENSOR_MODE_LIVE=1`, `SENSOR_MODE_REPLAY=2`) added to `sensor_state.h`.
- `fg_mode` and `s200_mode` fields (`sensor_mode_t`) added to `sensor_state_t`; initialised to `SENSOR_MODE_MANUAL` in `sensor_state_init()`.

### Changed
- `firmware/sensorEmulator/main.cpp` — updated to Phase 5:
  - `nvs_cfg_init()` called after `rs485_init()` and before `sensor_state_init()`.
  - `nvs_cfg_load_all()` called after `sensor_state_init()` to overwrite hardcoded defaults with NVS values.
  - `fg6485a_slave_register()`, `s200_slave_register()`, and `modbus_slave_set_addrs()` now use NVS-loaded slave addresses instead of hardcoded 1 / 44.
  - Boot banner updated to "Phase 5 NVS Settings".

### Verified
- Fresh NVS (post-erase first boot): all defaults printed — `addr=1 mode=0 temp=250 hum=500` (FG6485A) and `addr=44 mode=0 spd=5000 dir=180000` (S200).
- `nvs_cfg_set_i16("fg_temp_manual", 333)` → subsequent boot reads `temp=333`; FC03 response reg 0x0001 = `0x014D` = 333.
- Reboot without write: `temp=333` retained (NVS partition persists across software resets).
- Firmware reflash without full erase: `temp=333` retained (NVS partition at separate flash address, not touched by `pio run -t upload`).
- All three master Modbus frames continue to be answered correctly.
- Build: SUCCESS, 0 warnings, RAM 7.3 %, Flash 23.1 %.

---

## [0.4.0] — 2026-05-04  Phase 4: S200 Emulation

### Added
- `firmware/sensorEmulator/sensors/s200_slave.h` / `s200_slave.cpp` — SenseCAP S200 Modbus slave handlers:
  - FC04 serving wind direction min/max/avg (regs 0x0008–0x000D) and wind speed min/max/avg (regs 0x000E–0x0013): int32 × 1000, big-endian word order, defaults 180 000 (180.000°) and 5 000 (5.000 m/s).
  - FC04 serving heating temperature high/low (regs 0x001C–0x001F): int32 × 1000, default 25 000 (25.000 °C).
  - FC03 serving config registers: slave address (reg 0x1000 = 44) and baud rate code (reg 0x1001 = 1).
  - Pre-mutex range validation using same validate-outside/read-inside pattern as FG6485A handlers.

### Changed
- `firmware/sensorEmulator/main.cpp` — updated to Phase 4; `s200_slave_register(44)` called in `setup()` alongside existing `fg6485a_slave_register(1)`.

### Verified
- Live Modbus master poll confirmed all three master frames now answered:
  - Frame 1 (FC03 addr 1 qty 2): `01F4 00FA` — 50.0 %RH, 25.0 °C ✅
  - Frame 2 (FC04 addr 44 qty 12): `0002BF20 × 3, 00001388 × 3` — wind dir 180.000°, speed 5.000 m/s ✅
  - Frame 3 (FC04 addr 44 reg 0x001C qty 2): `000061A8` — 25.000 °C heater ✅
- Frame 3 was previously suppressed by the master (it only sends heater query when Frame 2 succeeds); first observed in Phase 4.
- Build: SUCCESS, 0 warnings, RAM 7.3 %, Flash 22.4 %.

---

## [0.3.0] — 2026-05-04  Phase 3: FG6485A Emulation

### Added
- `firmware/sensorEmulator/sensors/sensor_state.h` / `sensor_state.cpp` — shared sensor state struct with FreeRTOS mutex, holding register values for both FG6485A and S200; `sensor_state_init()` populates all defaults.
- `firmware/sensorEmulator/sensors/fg6485a_slave.h` / `fg6485a_slave.cpp` — FG6485A Modbus slave handlers:
  - FC03 serving measurement regs 0x0000–0x0001 (humidity × 10, temperature × 10), static device info 0x0008–0x000B, and alarm config 0x000C–0x0013.
  - FC16 writing alarm config regs 0x000C–0x0013 and correction offsets 0x001D–0x001E.
  - Exception 0x02 returned for any register address outside defined ranges.
  - All reads/writes execute under the shared mutex; range validation runs outside the critical section.

### Changed
- `firmware/sensorEmulator/main.cpp` — updated to Phase 3; `sensor_state_init()` and `fg6485a_slave_register(1)` called in `setup()` before the slave task starts.

### Verified
- Live master first poll: FC03 addr 1 reg 0x0000 qty 2 → `01F4 00FA` (50.0 %RH, 25.0 °C). CRC `3A 7E` correct. Master accepted without retry.
- Build: SUCCESS, 0 warnings, RAM 7.3 %, Flash 22.4 %.

---

## [0.2.0] — 2026-05-04  Phase 2: Modbus Slave Skeleton

### Added
- `firmware/sensorEmulator/modbus/modbus_crc.h` / `modbus_crc.cpp` — CRC-16/IBM (poly 0xA001, init 0xFFFF).
- `firmware/sensorEmulator/modbus/modbus_slave.h` / `modbus_slave.cpp` — Modbus RTU slave:
  - Frame receiver accumulates bytes until 20 ms inter-frame silence (RS485_FRAME_TIMEOUT_MS).
  - CRC validation; green LED blink on valid frame, red blink on CRC error.
  - FC dispatch table (`modbus_register_handler`); exception 0x01 for unregistered FC.
  - `modbus_build_exception()` helper for exception 0x01 and 0x02.
  - `modbus_slave_task` runs at `configMAX_PRIORITIES − 1`.
- `firmware/platformio.ini` — PlatformIO project config: `espressif32@6.3.2`, `m5stack-atom`, FastLED, `upload_speed=1500000`.

### Changed
- `firmware/sensorEmulator/hal/rs485.cpp` — removed post-TX echo drain loop (hardware confirmed no self-echo); replaced `taskYIELD()` with `vTaskDelay(1)` in polling loops; raised `RS485_FRAME_TIMEOUT_MS` from 5 to 20.
- `firmware/sensorEmulator/main.cpp` — Phase 2 entry point: launches `modbus_slave_task`.

### Fixed
- Task watchdog crash: `taskYIELD()` at max FreeRTOS priority does not yield to IDLE. Fixed by using `vTaskDelay(1)` (1 tick = 10 ms on ESP-IDF 100 Hz tick rate).
- `vTaskDelay(pdMS_TO_TICKS(1))` truncates to 0 on the 100 Hz tick rate. Fixed by passing tick count directly: `vTaskDelay(1)`.
- Frame timeout fired before byte accumulation completed at 5 ms (< one tick period of 10 ms). Fixed by raising to 20 ms.

### Verified
- Live Modbus master (60 s poll cycle) sends FC03 addr 1 qty 2 and FC04 addr 44 qty 12; emulator returns exception 0x01 for both; master retries once per exception. CRC on all received frames validated correctly.

---

## [0.1.0] — 2026-05-04  Phase 1: Board Bringup & RS485 TX Verification

### Added
- `firmware/sensorEmulator/hal/led.h` / `led.cpp` — FastLED WS2812B wrapper (`led_init`, `led_set`, `led_blink`) on G27; blue on boot.
- `firmware/sensorEmulator/hal/rs485.h` / `rs485.cpp` — UART2 HAL: 9600 baud 8N1, RX G22, TX G19; `rs485_init`, `rs485_write`, `rs485_read_frame`.
- `firmware/sensorEmulator/main.cpp` — Phase 1 entry point: transmits frame `01 03 00 00 00 01 84 0A` every 2 s with green LED blink trigger.
- `firmware/implementationPlan.md` — 13-phase implementation plan.
- `firmware/implementationRealisation.md` — realisation log (findings, deviations, verified results per phase).

### Verified
- Scope confirms valid 9600-baud RS485 frames every 2 s on G19 TX line.
- RS485 self-echo loopback confirmed impossible on Atomic RS485 Base (RO disabled during TX by hardware auto-direction).
- USB serial shows `TX [N] 01 03 00 00 00 01 84 0A` every 2 s.
