# Replay Mode — User Manual

## Table of Contents

1. [Overview](#1-overview)
2. [Prerequisites](#2-prerequisites)
3. [CSV File Format](#3-csv-file-format)
   - 3.1 [Columns](#31-columns)
   - 3.2 [Timestamp format](#32-timestamp-format)
   - 3.3 [Sensor value ranges and clamping](#33-sensor-value-ranges-and-clamping)
   - 3.4 [Syntax rules](#34-syntax-rules)
   - 3.5 [Hard limits](#35-hard-limits)
4. [Uploading a CSV File](#4-uploading-a-csv-file)
5. [Enabling Replay Mode](#5-enabling-replay-mode)
6. [Transport Controls](#6-transport-controls)
   - 6.1 [Start](#61-start)
   - 6.2 [Pause / Play](#62-pause--play)
   - 6.3 [Prev / Next](#63-prev--next)
   - 6.4 [Stop](#64-stop)
7. [Event Window](#7-event-window)
8. [Elapsed Timer](#8-elapsed-timer)
9. [Replay States](#9-replay-states)
10. [Behaviour Details](#10-behaviour-details)
    - 10.1 [What happens on Start](#101-what-happens-on-start)
    - 10.2 [Row injection and skipped rows](#102-row-injection-and-skipped-rows)
    - 10.3 [What happens on Stop or Done](#103-what-happens-on-stop-or-done)
    - 10.4 [Partial CSV columns](#104-partial-csv-columns)
11. [Limitations](#11-limitations)
12. [CSV Examples](#12-csv-examples)
    - 12.1 [Minimal — temperature and humidity only](#121-minimal--temperature-and-humidity-only)
    - 12.2 [Wind station simulation](#122-wind-station-simulation)
    - 12.3 [Both sensors, mixed presence](#123-both-sensors-mixed-presence)
    - 12.4 [One-hour storm simulation](#124-one-hour-storm-simulation)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. Overview

Replay mode lets you feed pre-recorded sensor data to the Modbus emulator from
a plain-text CSV file stored on the device's SPIFFS filesystem.  The emulator
reads one row per second, injects the values from that row into the active
sensor registers, and responds to any Modbus master with those values until the
next row fires.

**Key characteristics:**

- Timestamps are *relative offsets from the moment you press Start*, expressed
  as `HH:MM:SS`.  No network access and no NTP synchronisation is required.
- Playback is independent of the system clock — the emulator keeps its own
  monotonic 1-second timer.
- The FG6485A and S200 sensors can be placed in Replay mode independently.
  You can replay only temperature/humidity while wind stays in Manual, or vice
  versa.
- Columns that are absent from the header, or that have an empty cell in a
  data row, are simply not updated.  Values from the previous row (or from
  the Manual defaults) remain on the Modbus bus.
- Out-of-range values are silently clamped to the physical limits of each
  sensor and a warning is printed to the serial console.

---

## 2. Prerequisites

| Requirement | Detail |
|-------------|--------|
| WiFi connection | The web UI must be reachable to upload a file and to operate the transport controls.  Either STA (station) mode connected to your network, or AP mode (connect your computer directly to the device's access point). |
| CSV file ready | Prepared and saved on your computer before upload. |
| Sensor mode set to Replay | Each sensor (FG6485A, S200) whose values you want to replay must have its Mode radio button set to **Replay** in the web UI. |

> **Tip** — You can set one sensor to Replay and leave the other in Manual.
> Only sensors in Replay mode will have their registers updated by the task.

---

## 3. CSV File Format

### 3.1 Columns

The first line of the file is the header.  Column names are
**case-sensitive**.  The recognised names are listed below.

| Column name | Sensor | Unit | Required? |
|-------------|--------|------|-----------|
| `timestamp` | — | `HH:MM:SS` | **Recommended** (rows without a valid timestamp are skipped during index build) |
| `fg_temp` | FG6485A | °C (float) | Optional |
| `fg_hum` | FG6485A | %RH (float) | Optional |
| `s200_spd` | SenseCAP S200 | m/s (float) | Optional |
| `s200_dir` | S200 | degrees ° (float) | Optional |
| `s200_heat` | S200 | °C (float) | Optional |

Unrecognised column names are silently ignored.  Column order does not
matter; the parser maps columns by header name.

A minimum useful file contains at least `timestamp` and one sensor column.

### 3.2 Timestamp format

```
HH:MM:SS
```

- `HH` — hours, two digits, 00–23.
- `MM` — minutes, two digits, 00–59.
- `SS` — seconds, two digits, 00–59.

The timestamp is the **elapsed time from Start** at which the row should be
injected.  A row with `timestamp = 00:00:00` fires immediately when Start is
pressed.

Rows must be sorted in **ascending timestamp order**.  The parser does not
re-sort; a row whose timestamp is earlier than the previously injected row
will be reached instantly (the elapsed timer already passed it).

### 3.3 Sensor value ranges and clamping

Values outside the valid physical range are clamped silently.  A warning is
printed to the serial console (`[replay] <field> clamped: <actual value>`).

| Column | Minimum | Maximum | Resolution in Modbus register |
|--------|---------|---------|-------------------------------|
| `fg_temp` | −40.0 °C | 120.0 °C | 0.1 °C |
| `fg_hum` | 0.0 %RH | 99.9 %RH | 0.1 %RH |
| `s200_spd` | 0.000 m/s | 60.000 m/s | 0.001 m/s |
| `s200_dir` | 0.000 ° | 360.000 ° | 0.001 ° |
| `s200_heat` | −40.000 °C | 85.000 °C | 0.001 °C |

Values are rounded to the register resolution during injection.  For example,
`fg_temp = 23.456` is stored as `234` in the register (truncated to 23.4 °C).

### 3.4 Syntax rules

| Rule | Detail |
|------|--------|
| Delimiter | Comma `,` |
| Encoding | UTF-8 or ASCII (no BOM) |
| Line endings | `\r\n` (Windows) or `\n` (Unix) — both accepted |
| Empty cells | Allowed — the corresponding register is not updated for that row |
| Comment lines | Lines beginning with `#` are skipped entirely |
| Blank lines | Skipped entirely |
| Quoted strings | Not supported — do not wrap values in `"` |
| Leading/trailing spaces in cells | Stripped before parsing |
| Max line length | 200 bytes (including the newline) |

### 3.5 Hard limits

| Limit | Value |
|-------|-------|
| Maximum data rows | **2 900 rows** |
| Maximum file size | **200 KB** (upload rejected above this) |
| Max CSV line length | **200 bytes** |
| SPIFFS file path | `/replay.csv` (fixed, overwritten on each upload) |

> Rows beyond the 2900-row limit are **not** read.  Rows with no valid
> timestamp are skipped during index build and never injected.

---

## 4. Uploading a CSV File

1. Open the web UI in a browser (`http://<device-ip>/` or
   `http://sensor-emulator.local/`).
2. Scroll to the **Replay CSV** section (between the S200 sensor card and
   System Settings).
3. Click **Choose File** and select your `.csv` file.
4. Click **Upload CSV**.
5. Wait for the confirmation message showing the number of bytes written.

If a replay is already running when you upload, it is stopped automatically
before the file is overwritten.

**What gets stored**: the raw CSV bytes are written verbatim to `/replay.csv`
on SPIFFS.  The path is also saved to NVS so it survives a reboot.

---

## 5. Enabling Replay Mode

The sensor mode must be set **before or after** uploading — order does not
matter.

1. In the **FG6485A** section of the web UI, select the **Replay** radio button.
2. In the **S200** section of the web UI, select the **Replay** radio button.

Set only the sensor(s) whose data you want to replay.  A sensor in Manual or
Live mode is not affected by the replay task.

---

## 6. Transport Controls

The Replay CSV section contains five transport buttons.

### 6.1 Start

- Builds the in-memory row index (full CSV scan — takes a fraction of a second
  for typical files).
- Resets the elapsed timer to `00:00:00`.
- Injects the first row (timestamp `00:00:00`) immediately if one exists.
- Begins advancing the elapsed timer at 1 second per second.
- Can be pressed from **Idle**, **Done**, or **Error** states.
  Pressing Start while already Running has no effect (use Stop first).

### 6.2 Pause / Play

- **Pause** — freezes the elapsed timer; the Modbus registers retain the last
  injected values.  Transport buttons **Prev** and **Next** become enabled.
- **Play** — resumes the timer from the current elapsed position; playback
  continues with the next row whose timestamp has not yet been reached.

The button label toggles between "⏸ Pause" and "▶ Play" to reflect the
current state.

### 6.3 Prev / Next

Only available while **Paused**.

- **▶▶ Next** — advances to the immediately following row, injects its values,
  and updates the elapsed timer to that row's timestamp.  Disabled at the
  last row.
- **◀◀ Prev** — retreats to the immediately preceding row, injects its values,
  and updates the elapsed timer to that row's timestamp.  Disabled at the
  first row.

Navigation is O(1): the task uses the pre-built row index to seek directly
to any row without re-scanning the file.

### 6.4 Stop

Aborts playback from any state (Running, Paused, Done, Error) and returns the
task to **Idle**.  The Modbus registers keep the last injected values.  No
values are reset to defaults automatically — switch the sensor mode to Manual
or Live if you want to override them.

---

## 7. Event Window

The **event window** table in the Replay CSV section shows three rows of
context around the current playback position:

| Row | Description |
|-----|-------------|
| ↑ Previous | The row immediately before the current row (empty at row 0) |
| → **Current** (highlighted in blue) | The last injected row |
| ↓ Next | The row immediately after the current row (empty at the last row) |

Columns displayed: **Time** (HH:MM:SS), **†** (marker), **Temp** (°C),
**Hum** (%RH), **Spd** (m/s), **Dir** (°), **Heat** (°C).

A cell shows `—` when the column is absent from the CSV header, or when the
cell was empty in that row.

The window updates automatically via WebSocket on every row change, including
during Prev/Next navigation while paused.

The window is hidden while the replay task is in Idle state.

---

## 8. Elapsed Timer

The elapsed timer (`00:00:00` format) is displayed in the Replay CSV section.
It shows how many seconds have passed since Start was pressed, counting
independently of wall-clock time.

- Counting starts at `00:00:00` when Start is pressed.
- The timer **freezes** while Paused.
- Pressing **Prev** or **Next** while Paused sets the timer to the
  navigated row's timestamp.
- Pressing **Stop** resets the displayed value to `00:00:00`.

---

## 9. Replay States

| State | Meaning | Allowed commands |
|-------|---------|-----------------|
| **Idle** | No file loaded or stopped | Start |
| **Running** | Actively advancing through rows | Stop, Pause |
| **Paused** | Timer frozen | Stop, Play, Prev, Next |
| **Done** | Reached end of file | Start, Stop |
| **Error** | File open or parse failure | Start (try re-upload first), Stop |

The current state is shown in the `replay-status` line below the transport
controls.  It is also included in every WebSocket status push under
`status.replay.state`.

---

## 10. Behaviour Details

### 10.1 What happens on Start

1. Any previously running replay is stopped.
2. The CSV file `/replay.csv` is opened from SPIFFS.
3. The parser scans the entire file and builds an array of up to 2 900
   `(file_offset, ts_s)` index entries in heap memory (~23 KB for a full file).
4. The elapsed timer is set to 0.
5. If row 0 has `timestamp = 00:00:00` (or any timestamp ≤ 0 s), it is
   injected immediately.
6. The task enters **Running** state and advances the timer once per second
   using a FreeRTOS tick-count deadline to avoid drift from command processing.

### 10.2 Row injection and skipped rows

Each second the task checks whether the *next uninjected* row's timestamp has
been reached.  If `elapsed_s >= row.ts_s`, the row is injected and the pointer
advances.  If a file contains rows at `00:01:00`, `00:01:05`, and `00:01:10`
and the timer jumps from 59 s to 60 s, only the `00:01:00` row fires in that
tick; the `00:01:05` row fires at 65 s, etc.  No rows are skipped due to
timer granularity as long as the CSV timestamps are at least 1 second apart.

If two or more rows share the same timestamp, all matching rows are injected
in sequence during the same tick; the last one wins in the register.

### 10.3 What happens on Stop or Done

- The in-memory row index is freed (heap released).
- The CSV file is closed.
- The task returns to **Idle**.
- Sensor registers are **not** reset — whatever value was last injected stays
  on the Modbus bus until you switch the sensor mode or upload and start a
  new file.

### 10.4 Partial CSV columns

A CSV that contains only `timestamp,fg_temp,fg_hum` will only update
temperature and humidity registers.  The S200 registers are untouched,
regardless of whether S200 mode is set to Replay or Manual.

Conversely, a CSV with `s200_spd` but no `fg_temp` column will never touch
FG6485A registers — even if FG6485A mode is Replay.  It is safe to set FG6485A
to Replay and omit FG6485A columns; the registers simply retain their last
manual or live-mode values.

---

## 11. Limitations

| Limitation | Detail |
|------------|--------|
| Maximum rows | 2 900.  Rows beyond this index are never injected. |
| Maximum file size | 200 KB.  Upload is rejected above this. |
| No automatic mode switch | The sensor mode (Manual / Live / Replay) is not changed automatically when you press Start or Stop.  You must set it in the web UI. |
| No loop / repeat | Playback stops at the last row and enters **Done** state.  Press Start again to replay from the beginning. |
| Single file slot | Only one file (`/replay.csv`) is stored.  A new upload overwrites the previous file. |
| SPIFFS size | Total SPIFFS partition is shared with web assets (`index.html`, `app.js`, `style.css`).  Practical CSV storage capacity is ≈ 500 KB after web assets. |
| No real-time timestamps | Timestamps are relative offsets, not clock times.  You cannot synchronise replay to a wall-clock schedule. |
| S200 min/max/avg alignment | During replay all three S200 registers (min, max, avg) for speed, direction, and heat are set to the same injected value. |
| Clamping is silent in the UI | Out-of-range values are clamped and a warning is printed to the **serial console** only — the web UI does not show a clamping notification. |
| WiFi required for upload | File upload goes through the HTTP server; the device must be reachable over WiFi.  There is no SD card or USB mass-storage upload path. |

---

## 12. CSV Examples

### 12.1 Minimal — temperature and humidity only

A 5-minute ramp of temperature from 20 °C to 25 °C, humidity constant at
60 %RH.  Only the `timestamp`, `fg_temp`, and `fg_hum` columns are present;
the S200 registers are never touched.

```csv
timestamp,fg_temp,fg_hum
00:00:00,20.0,60.0
00:01:00,21.0,60.0
00:02:00,22.0,60.0
00:03:00,23.0,60.0
00:04:00,24.0,60.0
00:05:00,25.0,60.0
```

**Use case**: verifying that a greenhouse controller reacts to a slow
temperature rise while ignoring wind data.

---

### 12.2 Wind station simulation

Wind data only.  `fg_temp` and `fg_hum` columns are absent; only `s200_spd`
and `s200_dir` are driven.  Comments (`#`) are used to annotate phases.

```csv
timestamp,s200_spd,s200_dir
# Phase 1: calm morning
00:00:00,0.5,270
00:00:30,0.8,265
00:01:00,1.2,260
# Phase 2: wind picks up from west
00:02:00,3.5,280
00:02:30,5.0,285
00:03:00,7.2,290
# Phase 3: gust
00:04:00,12.0,295
00:04:10,14.5,300
00:04:20,11.0,295
# Phase 4: settles
00:05:00,6.0,285
00:06:00,4.0,275
```

**Use case**: testing a wind-triggered ventilation controller with a realistic
gust profile.

---

### 12.3 Both sensors, mixed presence

Not every row has values for every column.  Empty cells leave the
corresponding register unchanged.

```csv
timestamp,fg_temp,fg_hum,s200_spd,s200_dir,s200_heat
# Start of day — all values set
00:00:00,18.5,72.0,1.2,180,20.0
# Temperature and humidity update; wind unchanged
00:01:00,19.0,70.0,,,
# Wind update; temperature and humidity unchanged
00:02:00,,,2.5,185,
# Full update
00:03:00,19.5,68.0,3.0,190,19.5
# Only wind speed changes
00:04:00,,,4.2,,
00:05:00,,,5.8,,
# Back to a full update
00:06:00,20.0,66.0,5.0,195,19.0
```

**Use case**: simulating a real datalogger CSV where not all sensors are
sampled at the same rate.

---

### 12.4 One-hour storm simulation

A longer file demonstrating the approach for a fully automated sensor test
run.  Row spacing is 5 minutes to stay well within the 2 900-row limit.

```csv
timestamp,fg_temp,fg_hum,s200_spd,s200_dir
# Pre-storm: warm, humid, light breeze from south
00:00:00,28.0,85.0,1.5,180
00:05:00,27.5,87.0,2.0,175
00:10:00,27.0,90.0,3.0,170
# Storm approach
00:15:00,25.0,93.0,8.0,160
00:20:00,23.0,95.0,14.0,145
00:25:00,21.0,96.0,20.0,130
# Peak wind
00:30:00,19.5,97.0,28.0,120
00:35:00,19.0,98.0,32.0,115
# Storm passes
00:40:00,18.5,97.0,22.0,125
00:45:00,19.0,94.0,15.0,140
00:50:00,20.0,91.0,9.0,158
# Post-storm clear
00:55:00,21.5,88.0,5.0,170
01:00:00,23.0,84.0,2.5,178
```

The file has 13 data rows — well within limits.  Total playback time is
60 minutes.

**Use case**: running an automated 1-hour regression test of a greenhouse
controller's storm-response logic.

---

## 13. Troubleshooting

| Symptom | Likely cause | Resolution |
|---------|-------------|------------|
| Upload fails with "File too large" | CSV exceeds 200 KB | Split the file, reduce row count, or remove unused columns |
| Start button does nothing | No file has been uploaded yet, or SPIFFS write failed during upload | Upload a valid CSV and confirm the success message |
| State shows **Error** after Start | File is missing from SPIFFS (deleted or SPIFFS reformatted) or the file header is malformed | Re-upload the CSV; check that the first row is a valid header |
| Replay starts but no Modbus values change | The sensor mode is not set to **Replay** | Set FG6485A and/or S200 mode to Replay in the web UI |
| Values are wrong or clamped | CSV contains out-of-range values | Check the serial console for `[replay] <field> clamped:` messages; correct the CSV |
| Replay finishes instantly | All rows were skipped (no valid timestamps) | Ensure the `timestamp` column is present and values are in `HH:MM:SS` format |
| Row window shows `—` for all sensor cells | Those columns are not present in the CSV header | Add the desired column names to the header row |
| Event window table stays permanently empty (no rows appear at all) | Firmware older than 0.13.3 — `push_replay_window()` had a 96-byte entry buffer that was too small; the truncated output produced invalid JSON that the browser silently discarded | Update to firmware 0.13.3 or later |
| Values freeze mid-replay | Device restarted (power cycle or watchdog); WiFi issue causing false stop impression | Check WebSocket badge (Offline/Online); reload the page; re-upload and restart if needed |
| Prev / Next buttons are greyed out | The task is not in Paused state | Press **Pause** first |
| After Stop, Modbus still returns old replay values | Expected behaviour — Stop does not reset registers | Switch the sensor mode to Manual or set manual values via the sliders |
