# Modbus Sensor Emulator — Design Plan

## 1. Purpose

Emulate two Modbus RTU sensors concurrently on a single ESP32-based device
(M5Stack Atom Lite + Atomic RS485 Base):

| Sensor | Protocol | Values |
|---|---|---|
| **ASAIR FG6485A** | Modbus RTU, FC03, slave addr 1 (default) | Temperature (−40–120 °C), Humidity (0–99.9 %RH) |
| **Seeed SenseCAP S200** | Modbus RTU, FC04, slave addr 44 (default) | Wind speed min/avg/max (0–60 m/s), Wind direction min/avg/max (0–360°), Heating temp |

Both slaves share the same RS485 bus and answer only to their own slave address.

---

## 2. Sensor Emulation Modes

Each sensor has its own independent mode selection.

### 2.1 Manual
- All register values are set directly by the user through the web interface.
- Controls: slider or numeric input box — the slider value is always reflected in the input box. The value is only applied to the emulator after pressing **Apply**.
- **Input validation**: before a value is applied or stored, it is clamped to the sensor's physical range (see §11). If the entered value is below the minimum it is set to the minimum; if above the maximum it is set to the maximum. The clamped value is shown back in the input box.
- Values are stored in NVS so they survive a reboot.

### 2.2 Live *(internet required)*
- Values are fetched from the [Open-Meteo](https://open-meteo.com/) free API
  (no API key needed, rate limit: 1 000 calls/day on the free tier).
- Location resolution (used for the weather query):
  - **Priority 1**: When connected to the internet, latitude/longitude are resolved
    automatically from the public IP address via an IP-geolocation API.
  - **Priority 2**: Location is set manually (latitude / longitude) in the web
    interface and stored in NVS.
- A FreeRTOS timer task paces requests to stay within the daily limit
  (≤ 1 call every ~87 seconds).
- Mapping:
  - FG6485A ← `temperature_2m`, `relativehumidity_2m`
  - S200 ← `windspeed_10m`, `winddirection_10m`
    (min/max derived from the hourly spread; heating temp set to ambient)
- If the fetch fails the last known value is held.

### 2.3 Replay
- The user uploads a UTF-8 `.csv` file to the device (via the web interface).
- Format: `timestamp,fg_temperature,fg_humidity,...` — ISO-8601 timestamp column
  (local time, with or without a UTC offset suffix).
- A FreeRTOS task advances through the file and makes each value active at its
  timestamp.  Comparison is done in **local time** (timezone resolved from
  `live_lat`/`live_lon` including DST — see §8.2), so the device clock must be
  set and the timezone must be configured before playback starts.
- **Input validation during replay**: each value read from a CSV row is clamped
  to the sensor's physical range (see §11) before being written to
  `sensor_state`.  Out-of-range rows are not skipped — the clamped value is
  used and a warning is logged.
- The active row is highlighted in the web interface.
- The file is stored in SPIFFS/LittleFS; the filename is saved in NVS.

---

## 3. Hardware

- **MCU**: ESP32 (M5Stack Atom Lite — ESP32-PICO-D4)
- **RS485 interface**: Atomic RS485 Base
  - UART2 RX → G22, TX → G19 (direction control is hardware-automatic)
  - 9600 baud, 8N1 — matches both sensors
- **RGB LED** (G27, FastLED):
  - **Blue** (steady) — normal operation, idle / listening
  - **Green** (single blink) — valid frame received (correct CRC)
  - **Red** (single blink) — frame received with wrong CRC
  - **Red** (continuous) — faulty operation / unrecoverable error

Pinout is identical to the existing test-client in
`documentation/code_modbusTestClient/`.

---

## 4. Firmware Architecture (FreeRTOS)

```
┌───────────────────────────────────────────────────────────┐
│                        FreeRTOS tasks                     │
│                                                           │
│  modbus_slave_task   ← highest priority                   │
│    Listens on UART2 / RS485                               │
│    Dispatches FC03 (FG6485A) and FC04 (S200) replies      │
│    Posts log entries to log_queue                         │
│                                                           │
│  fg6485a_mode_task                                        │
│    Executes manual / live / replay logic for FG6485A      │
│    Writes shared sensor_state.fg6485a under mutex         │
│                                                           │
│  s200_mode_task                                           │
│    Executes manual / live / replay logic for S200         │
│    Writes shared sensor_state.s200 under mutex            │
│                                                           │
│  live_fetch_task  (suspended when not in live mode)       │
│    Calls Open-Meteo API over HTTPS                        │
│    Rate-limited by a 87-second FreeRTOS timer             │
│                                                           │
│  replay_task  (suspended when not in replay mode)         │
│    Reads CSV from SPIFFS, advances by local clock time    │
│    (DST-aware — uses localtime_r() for row comparison)    │
│                                                           │
│  ntp_task                                                 │
│    Syncs SNTP on WiFi connect; updates system clock       │
│                                                           │
│  web_server_task                                          │
│    Serves HTTP + WebSocket for live UI updates            │
│    Handles form POSTs for settings / file upload          │
│                                                           │
│  wifi_manager_task                                        │
│    Starts AP ("SensorEmulator-<AABB>", no password)       │
│    Switches to client mode on successful STA connect      │
│    Registers mDNS hostname "sensor-emulator" on STA up    │
│    Reachable at http://sensor-emulator.local              │
│    Disables AP once STA link is up                        │
└───────────────────────────────────────────────────────────┘
```

### 4.1 Shared State

```c
typedef struct {
    // FG6485A (raw register values, ×10)
    int16_t  fg_temperature;   // reg 0x0001
    uint16_t fg_humidity;      // reg 0x0000

    // S200 (raw values, ×1000)
    int32_t  s200_dir_min;     // regs 0x0008-0x0009
    int32_t  s200_dir_max;     // regs 0x000A-0x000B
    int32_t  s200_dir_avg;     // regs 0x000C-0x000D
    int32_t  s200_speed_min;   // regs 0x000E-0x000F
    int32_t  s200_speed_max;   // regs 0x0010-0x0011
    int32_t  s200_speed_avg;   // regs 0x0012-0x0013
    int32_t  s200_heat_temp;   // regs 0x001C-0x001D

    SemaphoreHandle_t mutex;
} sensor_state_t;
```

---

## 5. Modbus Slave Behaviour

### FG6485A emulation
- Slave address: configurable (default 1), stored in NVS.
- Responds to **FC03** (Read Holding Registers):
  - `0x0000` → humidity raw (×10)
  - `0x0001` → temperature raw (×10, signed int16)
  - `0x0008–0x000B` → static device-info values
  - `0x000C–0x0013` → alarm config (read-back of last written values)
- Responds to **FC16** (Write Multiple Registers) for alarm config / corrections.
- Returns exception 0x01 for unsupported FC; exception 0x02 for out-of-range address.

### S200 emulation
- Slave address: configurable (default 44), stored in NVS.
- Responds to **FC04** (Read Input Registers):
  - `0x0008–0x001D` → wind direction, wind speed, heating temperature (×1000, int32 big-endian word order)
- Responds to **FC03** (Read Holding Registers) for config registers `0x1000–0x1001`.
- Returns standard Modbus exceptions for unknown FC or address.

---

## 6. Web Interface

Technology: **ESP-IDF HTTP server** + **WebSocket** for live push.
All pages are stored in SPIFFS as pre-built HTML/CSS/JS.

### Pages / Sections

#### Status (home)
- Live values for both sensors (WebSocket push, ~1 s interval).
- WiFi status: mode (AP / STA), SSID, IP address, RSSI.
- System clock (NTP-synced indicator).

#### Sensor Configuration
One card per sensor, identical layout:

| Control | Function |
|---|---|
| **Modbus slave address** input + **Apply** | Sets the slave address for this sensor (saved to NVS) |
| Radio: Manual / Live / Replay | Selects active mode |
| **Manual**: sliders + input boxes | Slider value is reflected in the input box; the value is only applied to the emulator after pressing **Apply**, which saves it to NVS.  The slider range and numeric input limits are constrained to the sensor's physical range (§11); any out-of-range value is clamped before it is applied |
| **Live**: location (lat/lon) input | Manual fallback location for Open-Meteo query; auto-resolved from public IP when internet is available (priority 1) |
| **Replay**: file upload + start/stop | Upload CSV, start playback, show active row |

#### WiFi Settings
- Scan & select SSID, enter password → **Connect**
- NTP server URL + **Sync now**
- Manual time/date input + **Set**

#### Modbus Log
- Scrolling table: timestamp, direction (RX/TX), raw hex frame, decoded summary.
- Posted via WebSocket from the `log_queue`.
- **Clear** button.

---

## 7. Persistent Settings (NVS)

| Key | Type | Default |
|---|---|---|
| `fg_slave_addr` | u8 | 1 |
| `fg_mode` | u8 | MANUAL |
| `fg_temp_manual` | i16 | 250 (= 25.0 °C) |
| `fg_hum_manual` | u16 | 500 (= 50.0 %RH) |
| `s200_slave_addr` | u8 | 44 |
| `s200_mode` | u8 | MANUAL |
| `s200_spd_manual` | i32 | 5000 (= 5.000 m/s) |
| `s200_dir_manual` | i32 | 180000 (= 180.000°) |
| `live_lat` | float | 52.37 (Amsterdam) |
| `live_lon` | float | 4.90 |
| `replay_file` | str | "" |
| `wifi_ssid` | str | "" |
| `wifi_pass` | str | "" |
| `ntp_server` | str | "pool.ntp.org" |
| `tz_posix` | str | "" (auto-resolved from `live_lat`/`live_lon`) |

---

## 8. Time Management

### 8.1 Clock source

1. **NTP (priority 1)**: On WiFi STA connect, `sntp_setoperatingmode` /
   `sntp_setservername` / `sntp_init()` are called. The NTP server is
   configurable in the web interface (default `pool.ntp.org`). NTP always
   delivers **UTC**; the system clock is kept in UTC internally.
2. **Manual (priority 2)**: Web interface POST sets the system time via
   `settimeofday()`. Used when no WiFi or NTP is unavailable.

### 8.2 Local time determination

All internal timestamps are UTC. Local time (shown in the web interface and
used as the reference clock for Replay mode) is derived at display / comparison
time using the following approach:

1. **Timezone resolution from location**: The device already stores
   `live_lat` / `live_lon` in NVS (used by Live mode for the Open-Meteo
   query). These coordinates are also used to resolve the IANA timezone
   identifier (e.g. `Europe/Amsterdam`) for the device's physical location.
2. **POSIX TZ string**: The resolved timezone is converted to a POSIX TZ
   expression (e.g. `CET-1CEST,M3.5.0,M10.5.0/3`) and applied via
   `setenv("TZ", tz_string, 1); tzset()`. The ESP-IDF `<time.h>` / `localtime_r()`
   functions then handle all UTC-to-local conversion including **daylight
   saving time (DST)** transitions automatically.
3. **Fallback**: If the timezone cannot be resolved from the stored coordinates
   (e.g. first boot with no internet), the system defaults to UTC
   (`setenv("TZ", "UTC0", 1)`). The correct timezone is applied as soon as
   coordinates are available.
4. **Manual override**: The user can enter a POSIX TZ string directly in the
   web interface WiFi/Time settings page; it is stored in NVS key `tz_posix`
   and takes precedence over the auto-resolved value.

### 8.3 Display

Current local time (with DST-corrected offset) is displayed in the web
interface header via WebSocket push (~1 s interval). An NTP-synced indicator
is shown alongside the clock.

---

## 9. WiFi Manager

```
Boot
 └─► Start AP "SensorEmulator-<last2MACbytes>" (open, no password)
     └─► Try STA connect if NVS credentials present
         ├─ Success → disable AP, operate in STA mode
         └─ Fail / no credentials → remain in AP mode
             └─► User configures STA via web interface
                 └─ Connect success → disable AP
```

- AP SSID format: `SensorEmulator-` + uppercase hex of last 2 bytes of
  `esp_wifi_get_mac(WIFI_IF_AP, mac)`, e.g. `SensorEmulator-A4B7`.
- AP IP: `192.168.4.1` (ESP-IDF default).
- On successful STA connection, the device registers itself via mDNS as
  `emulator.local`, making the web interface reachable at `http://emulator.local`
  without knowing the assigned IP address.
- Web interface is accessible in both AP and STA mode simultaneously until
  AP is disabled.

---

## 10. CSV Replay File Format

```
timestamp,fg_temperature,fg_humidity,s200_speed_avg,s200_dir_avg
2026-01-15T08:00:00,21.4,55.2,3.7,270
2026-01-15T08:05:00,21.6,54.8,4.1,265
```

- Header row is mandatory; columns are order-independent.
- Columns not present in a row retain their previous value.
- Timestamps are **local time** (no UTC offset suffix required).  The replay
  task parses each row's timestamp with `strptime()` and compares it to the
  current local time obtained via `localtime_r()`.  This means DST transitions
  are handled automatically: a row timestamped at `02:30:00` on a clock-change
  night is activated at the correct local moment, not an hour early or late.
- The device timezone (§8.2) must be configured and the clock must be set
  (NTP or manual) before playback starts; otherwise the replay task logs a
  warning and defers activation until the clock is valid.
- A new upload replaces the existing file and resets playback to the first row.

---

## 11. Input Validation & Clamping

All sensor value inputs — whether entered via the web interface (Manual mode)
or read from a CSV file (Replay mode) — are validated against the physical
range of the sensor.  Values outside the range are **clamped** to the nearest
limit (minimum or maximum) before being applied to `sensor_state` or stored
in NVS.

### 11.1 Sensor value ranges

| Sensor | Field | Physical min | Physical max | Raw encoding |
|---|---|---|---|---|
| FG6485A | Temperature | −40 °C | 120 °C | `int16_t` ×10 (−400 … 1200) |
| FG6485A | Humidity | 0 %RH | 99.9 %RH | `uint16_t` ×10 (0 … 999) |
| S200 | Wind speed (min/avg/max) | 0 m/s | 60 m/s | `int32_t` ×1000 (0 … 60 000) |
| S200 | Wind direction (min/avg/max) | 0° | 360° | `int32_t` ×1000 (0 … 360 000) |
| S200 | Heating temperature | −40 °C | 85 °C | `int32_t` ×1000 (−40 000 … 85 000) |

### 11.2 Clamping rules

- Clamping is applied **before** writing to `sensor_state` and **before** NVS commit.
- The clamped value (not the original) is stored and served over Modbus.
- In the web interface, the slider widget is bounded to [min, max]; the numeric
  input box accepts free entry but the **Apply** POST handler clamps the value
  server-side.  The WebSocket status push reflects the clamped value so the
  UI always shows what is actually being emulated.
- During CSV replay, each field is clamped independently.  A warning entry is
  written to the Modbus log for every row that contained an out-of-range value.

---

## 12. Sequence of Development

1. **Board bringup** — UART2 RS485 loopback, LED feedback (re-use test-client
   patterns from `documentation/code_modbusTestClient/`).
2. **Modbus slave skeleton** — static register values, CRC, exception handling.
3. **FG6485A emulation** — FC03/FC16 register map from
   `documentation/code_FG6485A_driver/`.
4. **S200 emulation** — FC04 register map from
   `documentation/code_S200_driver/`.
5. **NVS + settings** — persist all configurable values.
6. **WiFi manager** — AP/STA switching, SSID generation.
7. **Web interface** — HTTP server, WebSocket live push, status + config pages.
8. **Manual mode** — wire UI controls to shared state.
9. **NTP + manual time** — time management tasks.
10. **Live mode** — Open-Meteo HTTPS fetch, rate limiter.
11. **Replay mode** — CSV parser, time-driven playback.
12. **Modbus log** — log queue → WebSocket → UI table.
13. **Integration testing** — use master from `documentation/code_modbusTestClient/`
    as the test client.

---

## 13. Reference Documentation

| Path | Content |
|---|---|
| `documentation/code_FG6485A_driver/` | FG6485A register map, API, FreeRTOS polling task |
| `documentation/code_S200_driver/` | S200 register map, API, FreeRTOS polling task |
| `documentation/code_modbusTestClient/` | Working Modbus slave example on Atom Lite + RS485 Base |
| `documentation/T-RH_ FG6485A/FG6485A.md` | FG6485A sensor datasheet notes |
| `documentation/W-Sensecap-S200/S200.md` | S200 sensor datasheet notes |
