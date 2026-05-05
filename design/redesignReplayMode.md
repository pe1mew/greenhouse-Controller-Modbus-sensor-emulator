# Replay Mode Redesign Plan

**Date**: 2026-05-05  
**Supersedes**: Phase 11 replay design (real-world ISO-8601 timestamps, Start/Stop only)

---

## 1  Motivation

The Phase 11 replay mode has three significant limitations:

| Limitation | Impact |
|---|---|
| Timestamps are absolute wall-clock (`YYYY-MM-DDTHH:MM:SS`), requiring NTP sync | Cannot replay without internet/NTP; CSV files are date-stamped and single-use |
| Only Start and Stop; no pause or manual step | Cannot inspect individual events, freeze a moment, or position playback |
| No visual context of adjacent events | Operator cannot see what was last injected or what is coming next |

The redesign replaces absolute timestamps with relative offsets, adds a full transport control set (Start / Prev / Pause·Play / Next / Stop), and adds a 3-row event context window.

---

## 2  CSV Format

### 2.1  Header and columns

```
timestamp,fg_temp,fg_hum,s200_spd,s200_dir,s200_heat
```

| Column | Type | Unit | Required |
|--------|------|------|----------|
| `timestamp` | `HH:MM:SS` | seconds offset from start | **Yes** |
| `fg_temp` | float | °C | No |
| `fg_hum` | float | %RH | No |
| `s200_spd` | float | m/s | No |
| `s200_dir` | float | ° | No |
| `s200_heat` | float | °C | No |

All sensor columns are optional both in the header and per-cell.  Absent header
columns are never injected.  A column present in the header but empty in a row
means "do not change this sensor value for this event" — the previous value is
preserved.

### 2.2  Timestamp semantics

`HH:MM:SS` is a **relative offset** from the moment the user presses **Start**.
`00:00:00` is the start origin.  The timer starts at zero when Start is pressed
and advances in real time.  When the elapsed timer reaches a row's timestamp
the row is injected.

```
timestamp,fg_temp,fg_hum,s200_spd,s200_dir,s200_heat
00:00:00,25.0,60,,180,         ← injected immediately at start
00:01:30,26.5,58,3.2,,         ← injected 1 min 30 s after start
00:05:00,,,5.0,270,20.5        ← injected 5 min after start
```

Timestamp `00:00:00` is valid and is injected the moment playback begins.
Rows with timestamps before `00:00:00` are skipped (impossible — all offsets ≥ 0).

### 2.3  Capacity

- Maximum rows: **2900** data rows (plus one header row).
- Maximum timestamp: `23:59:59` (86 399 s).
- Typical file size at 2900 rows × 40 bytes average = **~116 KB** — within the
  current 200 KB SPIFFS upload limit.

### 2.4  Comments and blank lines

Lines beginning with `#` and empty lines are silently skipped (same as Phase 11).

---

## 3  Playback Semantics

### 3.1  State machine

```
            Start
  IDLE ─────────────► RUNNING
   ▲                  │    ▲
   │   Stop           │    │ Play
   │◄─────────────────┤    │
   │                Pause  │
   │                  │    │
   │                  ▼    │
   │◄───────────── PAUSED ─┘
   │  Stop
   │
   │   EOF reached
   ├◄──────────────── DONE (→ auto returns to IDLE after 2 s)
   │
   └◄──────────────── ERROR (file open / parse failure)
```

| State | Timer | Injection | Next/Prev |
|-------|-------|-----------|-----------|
| IDLE | stopped, reset | — | disabled |
| RUNNING | advancing | automatic at row timestamp | disabled |
| PAUSED | frozen | on Next/Prev | enabled |
| DONE | stopped | — | disabled |
| ERROR | stopped | — | disabled |

### 3.2  Timer

`elapsed_s` is a `uint32_t` counting whole seconds since Start.  The replay
task increments it once per second via `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))`
when in RUNNING state.  The task checks whether the next uninjected row's
`ts_s ≤ elapsed_s`; if so it injects the row and advances the row pointer.

### 3.3  Pause behaviour

When the user presses **Pause**:
- The timer (`elapsed_s`) is frozen at its current value.
- Row injection halts — no further rows are processed automatically.
- **Next** and **Previous** become active.

### 3.4  Next / Previous (paused state only)

**Next**: advance `current_row` by one (if not already at last row), inject
that row's sensor values, and set `elapsed_s` to that row's `ts_s`.

**Previous**: move `current_row` back by one (if not already at row 0), inject
that row's sensor values, and set `elapsed_s` to that row's `ts_s`.

This gives the operator full manual scrubbing through the timeline.  The
elapsed time display tracks the timestamp of the currently displayed row.

### 3.5  Play (resume from paused)

When the user presses **Play** from PAUSED:
- The timer resumes from the frozen `elapsed_s` (which equals the current
  row's `ts_s` after navigation).
- The next injection pointer is set to `current_row + 1`.
- The task transitions to RUNNING.

### 3.6  Stop

From any non-IDLE state: resets `elapsed_s` to 0, `current_row` to 0 (no
injection), transitions to IDLE.

### 3.7  Start

From IDLE: resets `elapsed_s` to 0, `current_row` to -1 (before first row),
opens the CSV file, builds the row index, transitions to RUNNING.  The first
row with `ts_s == 0` is injected immediately.

---

## 4  In-Memory Row Index

To support O(1) random access for Next/Previous without re-parsing the file
from the beginning, a compact index is built once when Start is pressed.

### 4.1  Index entry

```c
typedef struct {
    uint32_t file_offset;  /* byte offset of this row in the SPIFFS file */
    uint32_t ts_s;         /* timestamp in seconds                        */
} replay_index_entry_t;
```

Size: 8 bytes × 2900 = **23.2 KB** heap allocation.  Available RAM after
Phase 12: ~277 KB free → well within budget.

### 4.2  Build procedure

After `csv_open()`, iterate all rows via `csv_next_row()`, record the file
position before each parse call (`File.position()`) and the parsed `ts_s`.
Store into the index array.  Total rows stored in `s_row_count`.  Then rewind
to row 0 for playback.

### 4.3  Seeking to a row

```c
f.seek(s_index[row].file_offset);
csv_read_row_at(p, &row_data);   // parse one row without advancing further
```

This is used for Next/Previous injection and also for populating the event
window.

---

## 5  Event Context Window

A 3-row strip in the web UI shows the adjacent events around the current
position:

```
┌─────────────┬───────────┬───────┬──────┬───────┬───────┬─────────┐
│  Time       │           │ Temp  │ Hum  │  Spd  │  Dir  │  Heat   │
├─────────────┼───────────┼───────┼──────┼───────┼───────┼─────────┤
│ 00:01:30    │  prev     │ 25.0  │ 60   │  —    │ 180   │  —      │
│ 00:02:00  ► │  current  │ 26.0  │ 58   │  3.2  │  —    │  —      │
│ 00:03:00    │  next     │  —    │  —   │  4.1  │ 270   │ 20.5    │
└─────────────┴───────────┴───────┴──────┴───────┴───────┴─────────┘
```

- `prev`: the last injected / navigated-to row (row index `current_row - 1`).
  Shown as `-` when at row 0.
- `current`: the row most recently injected or navigated to (row index
  `current_row`).  Highlighted with `►` marker.
- `next`: the next row to be injected (row index `current_row + 1`).  Shown
  as `-` when at last row.

Empty cells (column absent or empty in CSV) display as `—`.

The window is pushed to the browser as a separate WebSocket message
`{type:"replay_window"}` whenever `current_row` changes (on each injection
during RUNNING, on each Next/Prev during PAUSED).  It is **not** included in
the 1 Hz status JSON to avoid inflating that payload.

---

## 6  WebSocket Protocol

### 6.1  Status JSON changes

`build_status_json()` `"replay"` object is extended:

```json
"replay": {
  "state":     "idle | running | paused | done | error",
  "row":       42,
  "row_count": 2900,
  "elapsed_s": 120
}
```

`elapsed_s` is the current timer value; `row_count` is the total number of
data rows in the loaded file (0 when IDLE with no file).

### 6.2  New event: `replay_window`

Pushed by `ws_push_task` whenever `current_row` changes, using
`ws_broadcast()` with a stack buffer (fits in ~400 bytes).

```json
{
  "type": "replay_window",
  "prev": {
    "row": 41,
    "ts":  "00:01:30",
    "fg_temp": 25.0,
    "fg_hum":  60,
    "s200_spd": null,
    "s200_dir": 180,
    "s200_heat": null
  },
  "curr": { "row": 42, "ts": "00:02:00", ... },
  "next": { "row": 43, "ts": "00:03:00", ... }
}
```

Absent sensor values (column not present in header, or empty cell) are `null`.
When `prev`/`next` does not exist (at boundaries) the corresponding key holds
`null` rather than an object.

The firmware fills the window by seeking to the relevant rows in the index and
parsing each row into a small stack struct; no heap allocation needed beyond
the broadcast buffer itself.

### 6.3  Notification flag

`replay_task` uses a volatile `bool s_window_dirty` flag.  It is set whenever
`current_row` changes.  `ws_push_task` checks this flag each cycle; if set it
reads the window data (under a lightweight spinlock / atomic copy) and broadcasts
the `replay_window` message, then clears the flag.

---

## 7  HTTP API Changes

`POST /replay/control` — extended action set:

| `action` | From state | Effect |
|---------|-----------|--------|
| `"start"` | IDLE | Reset timer; open file; build index; begin RUNNING |
| `"pause"` | RUNNING | Freeze timer; enable Next/Prev |
| `"play"` | PAUSED | Resume timer from `elapsed_s`; disable Next/Prev |
| `"next"` | PAUSED | Advance one row; inject; update `elapsed_s` |
| `"prev"` | PAUSED | Retreat one row; inject; update `elapsed_s` |
| `"stop"` | any | Reset all; return to IDLE |

Actions received in an invalid state are ignored (no error; `state` in
response reflects actual current state).

Response body (unchanged structure, new field):
```json
{ "ok": true, "state": "paused", "row": 42, "elapsed_s": 120 }
```

`POST /replay/upload` — no change.

---

## 8  Firmware Changes

### 8.1  `util/csv_parser.h` / `csv_parser.cpp`

| Change | Detail |
|--------|--------|
| Timestamp field type | `struct tm ts` → `uint32_t ts_s` (seconds 0–86399) |
| Timestamp parser | Remove `strptime`; add `sscanf(str, "%u:%u:%u", &h, &m, &s)` → `ts_s = h*3600 + m*60 + s` |
| Seek support | Expose `csv_tell(p)` returning `File.position()` before the most-recently-parsed row |
| Seek support | Expose `csv_seek(p, offset)` calling `File.seek(offset)` then parsing next row |
| Row count | Expose `csv_row_count(p)` (populated after a full scan, as done during index build) |
| `CSV_MAX_LINE` | Increase `160` → `200` to safely accommodate all 6 columns |

`csv_row_t` timestamp field:
```c
// Before (Phase 11)
struct tm  ts;
bool       has_ts;

// After (redesign)
uint32_t   ts_s;      /* seconds offset 0..86399; UINT32_MAX if not parsed */
bool       has_ts;
```

### 8.2  `tasks/replay_task.h` / `replay_task.cpp`

#### New public API

```c
typedef enum {
    REPLAY_IDLE    = 0,
    REPLAY_RUNNING = 1,
    REPLAY_PAUSED  = 2,   /* NEW */
    REPLAY_DONE    = 3,
    REPLAY_ERROR   = 4,
} replay_state_t;

void replay_task_init(void);

/* Control — all safe to call from any task context */
void replay_task_cmd_start(void);
void replay_task_cmd_stop(void);
void replay_task_cmd_pause(void);
void replay_task_cmd_play(void);
void replay_task_cmd_next(void);
void replay_task_cmd_prev(void);

/* Getters */
replay_state_t replay_task_get_state(void);
int            replay_task_get_row(void);
uint32_t       replay_task_get_elapsed_s(void);
int            replay_task_get_row_count(void);

/* Window — fills up to 3 entries; caller holds no lock */
typedef struct {
    bool     valid;
    int      row;
    uint32_t ts_s;
    float    fg_temp;  bool has_fg_temp;
    float    fg_hum;   bool has_fg_hum;
    float    s200_spd; bool has_s200_spd;
    float    s200_dir; bool has_s200_dir;
    float    s200_heat; bool has_s200_heat;
} replay_window_entry_t;

void replay_task_get_window(replay_window_entry_t *prev_out,
                            replay_window_entry_t *curr_out,
                            replay_window_entry_t *next_out);
```

Previous `replay_task_start()` and `replay_task_stop()` are replaced by the
`cmd_*` variants; callers in `web_server.cpp` are updated accordingly.

#### Internal command queue

A FreeRTOS queue of depth 4 carries `uint8_t` command tokens.  Commands are
dispatched at the top of the task's main loop, before the timer tick.  This
replaces the two volatile booleans used in Phase 11.

```c
typedef enum {
    CMD_START = 0,
    CMD_STOP,
    CMD_PAUSE,
    CMD_PLAY,
    CMD_NEXT,
    CMD_PREV,
} replay_cmd_t;
```

#### Task main loop sketch

```
loop:
    drain command queue
    if RUNNING:
        wait 1 s (ulTaskNotifyTake with 1000 ms timeout; woken early by command)
        elapsed_s++
        while index[next_inject_row].ts_s <= elapsed_s:
            seek + parse + inject row
            current_row = next_inject_row++
            set s_window_dirty
        if current_row >= row_count - 1: → DONE
    if PAUSED:
        wait indefinitely for command notification
        (Next/Prev processed by command handler above)
    if IDLE/DONE/ERROR:
        wait indefinitely for CMD_START
```

#### Index lifecycle

- Allocated (`malloc(row_count * sizeof(replay_index_entry_t))`) on CMD_START.
- Freed on CMD_STOP or on task re-start.
- `row_count` capped at `REPLAY_MAX_ROWS = 2900` during build; rows beyond
  the cap are silently ignored.

### 8.3  `web/web_server.cpp`

| Change | Detail |
|--------|--------|
| `handle_replay_control()` | Dispatch 6 actions: start, stop, pause, play, next, prev |
| `build_status_json()` | Add `elapsed_s` and `row_count` to `"replay"` object; `STATUS_JSON_MAX` raised `768 → 1024` |
| `ws_push_task` | After status push, if `s_window_dirty`, call new `push_replay_window()` helper |
| `push_replay_window()` | Calls `replay_task_get_window()`; formats JSON with `snprintf` into 512-byte stack buffer; calls `ws_broadcast()` |

### 8.4  `main.cpp`

- Boot banner updated to Phase 13 Replay Redesign.
- `replay_task_start()` / `replay_task_stop()` call sites → `replay_task_cmd_start()` / `replay_task_cmd_stop()`.

---

## 9  Web UI Changes

### 9.1  `data/index.html` — Replay CSV section

Replace the current two-button row with:

```html
<div class="row replay-controls">
  <button id="btn-replay-start"  onclick="replayCmd('start')">&#9654; Start</button>
  <button id="btn-replay-prev"   onclick="replayCmd('prev')" disabled>&#9664;&#9664; Prev</button>
  <button id="btn-replay-pause"  onclick="replayTogglePause()" disabled>&#9646;&#9646; Pause</button>
  <button id="btn-replay-next"   onclick="replayCmd('next')" disabled>&#9654;&#9654; Next</button>
  <button id="btn-replay-stop"   onclick="replayCmd('stop')" disabled>&#9632; Stop</button>
</div>
<p id="replay-elapsed" class="hint"></p>

<div id="replay-window">
  <table id="replay-win-tbl">
    <thead>
      <tr><th>Time</th><th></th><th>Temp</th><th>Hum</th><th>Spd</th><th>Dir</th><th>Heat</th></tr>
    </thead>
    <tbody>
      <tr id="replay-win-prev"><td colspan="7">—</td></tr>
      <tr id="replay-win-curr" class="replay-curr"><td colspan="7">—</td></tr>
      <tr id="replay-win-next"><td colspan="7">—</td></tr>
    </tbody>
  </table>
</div>
```

### 9.2  `data/app.js`

New / changed functions:

| Function | Purpose |
|----------|---------|
| `replayCmd(action)` | POST `/replay/control {action}`, update button states on response |
| `replayTogglePause()` | POST `pause` when RUNNING; POST `play` when PAUSED |
| `handleReplayStatus(r)` | Called from `handleStatus()`; updates elapsed display and button enable/disable |
| `handleReplayWindow(msg)` | Called from WebSocket `onmessage` for `type:"replay_window"`; renders 3-row window table |
| `fmtElapsed(s)` | Format seconds → `HH:MM:SS` string for display |
| `fmtRowCell(entry, field)` | Returns `"—"` for null, otherwise formatted value |

**Button enable/disable logic**:

| State | Start | Prev | Pause/Play | Next | Stop |
|-------|-------|------|------------|------|------|
| IDLE | ✅ | ❌ | ❌ | ❌ | ❌ |
| RUNNING | ❌ | ❌ | ✅ (shows Pause) | ❌ | ✅ |
| PAUSED | ❌ | ✅* | ✅ (shows Play) | ✅* | ✅ |
| DONE | ✅ | ❌ | ❌ | ❌ | ❌ |
| ERROR | ✅ | ❌ | ❌ | ❌ | ❌ |

\* Prev disabled when `row == 0`; Next disabled when `row == row_count - 1`.

### 9.3  `data/style.css`

```css
/* Replay transport buttons */
.replay-controls { flex-wrap: wrap; gap: .4rem; }

/* Event window */
#replay-window { margin-top: .6rem; }
#replay-win-tbl { width: 100%; table-layout: fixed; font-size: .82rem; }
#replay-win-tbl th, #replay-win-tbl td { padding: 3px 6px; }
#replay-win-tbl th:nth-child(1) { width: 80px; }
#replay-win-tbl th:nth-child(2) { width: 54px; }
tr.replay-curr { background: rgba(52,152,219,.18); font-weight: 600; }
tr.replay-curr::before { content: "►"; margin-right: .3rem; }
```

---

## 10  Memory and Performance Budget

| Item | Value |
|------|-------|
| Row index heap (`replay_index_entry_t[2900]`) | **23.2 KB** |
| Window broadcast buffer (stack, `ws_push_task`) | 512 B |
| `STATUS_JSON_MAX` increase | 768 → 1024 B (stack in `ws_push_task`) |
| Total additional heap | ~23.2 KB |
| RAM after addition (estimated) | ~254 KB free / 328 KB total |
| Flash impact | Minimal — logic reduction (remove `strptime`) offsets additions |

The index is only allocated while a file is loaded and playback has been
started; between sessions it is freed.

---

## 11  Differences from Phase 11

| Aspect | Phase 11 | Redesign |
|--------|----------|----------|
| Timestamp format | `YYYY-MM-DDTHH:MM:SS` (absolute) | `HH:MM:SS` (relative offset) |
| NTP required | Yes — pre-flight check aborts if not synced | No |
| Transport controls | Start, Stop | Start, Prev, Pause/Play, Next, Stop |
| Manual navigation | Not possible | Full scrub when paused |
| Event window | None | 3-row prev/current/next display |
| Random row access | Sequential only (re-parse from start for Prev) | O(1) via file offset index |
| State machine states | IDLE, RUNNING, DONE, ERROR | + PAUSED |
| WebSocket window event | Not present | `{type:"replay_window"}` |
| CSV column compatibility | Same column names | Same column names — fully backward compatible with Phase 11 header rows (only timestamp format changes) |

---

## 12  Implementation Sequence

1. **`util/csv_parser`** — update timestamp parsing (`HH:MM:SS` → `ts_s`), add `csv_tell()` / `csv_seek()`.
2. **`tasks/replay_task`** — full redesign: new state machine, command queue, index build, elapsed timer, Next/Prev injection, window API, `s_window_dirty` flag.
3. **`web/web_server.cpp`** — update `handle_replay_control()`, `build_status_json()`, add `push_replay_window()`, update `ws_push_task` drain.
4. **`data/index.html`** — replace replay button row, add event window table.
5. **`data/app.js`** — `replayCmd()`, `replayTogglePause()`, `handleReplayWindow()`, button state logic.
6. **`data/style.css`** — transport button row, event window styles.
7. **`webMock/server.py`** — update `_state` (`elapsed_s`, `row_count`, `replay_window`), handle new actions, push `replay_window` WS events.
8. **`main.cpp`** — update `replay_task_start/stop` call sites to `cmd_start/stop`.
9. Build, flash firmware + SPIFFS, verify.

---

## 13  Open Questions

| # | Question | Default assumption |
|---|----------|--------------------|
| 1 | Should `00:00:00` rows be injected before the very first Modbus poll cycle, or should Start wait for the first poll? | Inject immediately at Start, before any poll. |
| 2 | When navigating Prev/Next while paused, should sensor values be immediately reflected in Modbus responses? | Yes — `inject_row()` writes to `g_sensor_state` unconditionally. |
| 3 | Should the elapsed timer show sub-second precision in the UI? | No — whole-second display (`HH:MM:SS`) is sufficient. |
| 4 | Should uploading a new CSV while RUNNING/PAUSED stop playback first? | Yes — `handle_replay_upload()` calls `replay_task_cmd_stop()` before writing (same as Phase 11). |
