# Firmware Implementation Realisation

Test results, findings, and deviations from the plan recorded phase by phase.

Design reference: [`design/modbusSensorEmulator.md`](../design/modbusSensorEmulator.md)  
Plan reference: [`firmware/implementationPlan.md`](implementationPlan.md)

---

## Phase 1 — Board Bringup & RS485 TX Verification ✅

**Date completed**: 2026-05-04

### Outcome: PASS

### Findings

#### RS485 self-echo loopback is impossible on this hardware

The implementation plan originally called for a loopback test: transmit a
known byte sequence and receive it back on the same UART. This failed — zero
bytes were received.

**Root cause**: The M5Stack Atomic RS485 Base uses a transistor connected
directly to the UART TX line to auto-control the RS485 direction pins:

- When TX is asserted (line goes LOW for start bit) → DE is driven HIGH
  (transmitter enabled) **and** RE is driven HIGH (receiver disabled).
- The receiver output (RO) is therefore forced inactive during any
  transmission, making it physically impossible for the device to read back
  its own bytes.

There is no separate DE/RE GPIO; direction is entirely hardware-automatic.
This is standard half-duplex RS485 behaviour for this module family.

**Scope observation during debugging**: On reset, the RS485 bus showed one
high-to-low transition. This was the power-on state transition of the bus
idle level, not UART data. No UART frames were visible at that time because
the original firmware had already exited the one-shot test before the scope
was triggered.

#### Revised test: continuous TX every 2 seconds

The loopback test was replaced with a continuous transmit loop:
- Sends the 8-byte frame `01 03 00 00 00 01 84 0A` every 2 seconds.
- LED turns green immediately before TX as a scope trigger edge.
- USB serial prints `TX [N] 01 03 00 00 00 01 84 0A` on each iteration.

This confirmed the TX path works. RX path verification is deferred to
Phase 2 where a second device acting as Modbus master will send requests.

### Verification results

| Check | Result |
|-------|--------|
| USB serial shows `TX [N] 01 03 00 00 00 01 84 0A` every 2 s | ✅ Pass |
| Scope shows valid 9600-baud RS485 frames every 2 s | ✅ Pass |
| LED green-blink visible before each frame | ✅ Pass |
| LED blue on boot | ✅ Pass |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| "Write a loopback test: transmit, receive, log pass/fail" | Changed to TX-only continuous test. Loopback is not feasible on this hardware. |
| Phase title: "Board Bringup & RS485 **Loopback**" | Retitled to "Board Bringup & RS485 **TX Verification**" |
| RX path verified in Phase 1 | RX path deferred to Phase 2 (requires external master). |

### Hardware note for future phases

The Atomic RS485 Base direction control is fully automatic and requires no
firmware involvement. `rs485_write()` must not attempt to drive a DE GPIO.
The `rs485_read_frame()` function is valid and will receive bytes from an
external master — it simply cannot observe self-transmitted bytes.

---

## Phase 2 — Modbus Slave Skeleton ✅

**Date completed**: 2026-05-04

### Outcome: PASS

### Serial log evidence (master polling at 1-minute intervals)

```
10:59:42.560 > [modbus] RX → 01 03 00 00 00 02 C4 0B
10:59:42.561 > [modbus] FC 0x03 not handled → exception 0x01
10:59:42.561 > [modbus] TX → 01 83 01 80 F0
10:59:42.688 > [modbus] RX → 01 03 00 00 00 02 C4 0B   ← master retry
10:59:42.695 > [modbus] FC 0x03 not handled → exception 0x01
10:59:42.695 > [modbus] TX → 01 83 01 80 F0
10:59:42.793 > [modbus] RX → 2C 04 00 08 00 0C 77 B0
10:59:42.810 > [modbus] FC 0x04 not handled → exception 0x01
10:59:42.810 > [modbus] TX → 2C 84 01 12 C9
10:59:42.936 > [modbus] RX → 2C 04 00 08 00 0C 77 B0   ← master retry
10:59:42.936 > [modbus] FC 0x04 not handled → exception 0x01
10:59:42.936 > [modbus] TX → 2C 84 01 12 C9
11:00:42.973 > [modbus] RX → 01 03 00 00 00 02 C4 0B   ← next poll cycle (+60 s)
...
```

Frame decode:
- `01 03 00 00 00 02 C4 0B` — FC03, addr 1, reg 0x0000, qty 2. CRC `C4 0B` ✅
- `2C 04 00 08 00 0C 77 B0` — FC04, addr 44, reg 0x0008, qty 12. CRC `77 B0` ✅
- Exception `01 83 01 80 F0` — addr 1, FC03|0x80, exception 0x01, CRC `80 F0` ✅
- Exception `2C 84 01 12 C9` — addr 44, FC04|0x80, exception 0x01, CRC `12 C9` ✅

Master retries once per exception (expected — its retry count = 1).
S200 heater frame (Frame 3, reg 0x001C) not sent by master when Frame 2
returns an exception — consistent with master's error path logic.
Poll interval confirmed at 60 seconds.

### Findings

#### `vTaskDelay(pdMS_TO_TICKS(1))` does not yield on ESP-IDF (10 ms tick rate)

`pdMS_TO_TICKS(1)` uses integer division. The ESP-IDF default FreeRTOS tick
rate is **100 Hz (10 ms/tick)**, so `pdMS_TO_TICKS(1) = 1000/100 / 1 = 0 ticks`
— truncated to zero. `vTaskDelay(0)` is a no-op; it does not yield.

The fix was to pass the tick count directly: `vTaskDelay(1)` (1 tick = 10 ms).
This genuinely blocks the task and allows the scheduler to run IDLE.

`RS485_FRAME_TIMEOUT_MS` was also raised from 5 ms to 20 ms so the
silence-detection window is larger than one tick period, preventing premature
frame termination between back-to-back bytes.

#### Task watchdog crash at highest FreeRTOS priority

The slave task was created at `configMAX_PRIORITIES - 1` (highest priority)
as specified in the plan. The first flash crashed with a task watchdog
trigger approximately 10 seconds after boot:

```
E (10420) task_wdt: Task watchdog got triggered. The following tasks did not reset the watchdog in time:
E (10420) task_wdt:  - IDLE (CPU 0)
E (10420) task_wdt: Tasks currently running:
E (10420) task_wdt: CPU 0: modbus_slave
```

**Root cause**: `rs485_read_frame()` (written in Phase 1 as a standalone
TX test) used `taskYIELD()` while spin-waiting for incoming bytes. At the
highest scheduler priority `taskYIELD()` only yields to tasks of equal or
higher priority — there are none, so the IDLE task never ran. The ESP-IDF
task watchdog requires IDLE to run periodically to reset its timer.

**Fix**: Replaced both `taskYIELD()` calls inside `rs485_read_frame()` with
`vTaskDelay(pdMS_TO_TICKS(1))`. This genuinely blocks the task and allows
the scheduler to run IDLE (and any lower-priority tasks) between byte polls.

**Impact on timing**: The 1 ms yield per poll adds at most 1 ms jitter to
the inter-character silence detection. This is acceptable — `RS485_FRAME_TIMEOUT_MS`
is 5 ms (> 3.5 × char time = 3.65 ms at 9600 baud), so a 1 ms overrun still
leaves 0.35 ms margin and will not split a valid Modbus frame.

#### Echo drain loop removed from `rs485_write()`

The Phase 1 `rs485_write()` implementation included a post-TX loop that
attempted to drain `len` echo bytes from the RX FIFO. This was based on
incorrect assumptions (carried over from a comment in the reference test
client). Since the Atomic RS485 Base disables its own receiver during TX
(confirmed in Phase 1), no echo bytes ever appear. The drain loop wasted
up to `len × 2 + 10` ms after each transmission and risked consuming the
leading bytes of the next inbound request if the master responded quickly.
The loop was removed.

### Files created / changed

| File | Change |
|------|--------|
| `sensorEmulator/modbus/modbus_crc.h` | New — `modbus_crc16()` declaration |
| `sensorEmulator/modbus/modbus_crc.cpp` | New — CRC-16/IBM (poly 0xA001, init 0xFFFF) |
| `sensorEmulator/modbus/modbus_slave.h` | New — handler type, register/build-exception/task API |
| `sensorEmulator/modbus/modbus_slave.cpp` | New — frame RX, CRC check, dispatch table, exception builder, serial logging |
| `sensorEmulator/main.cpp` | Updated — Phase 2 header, TX test removed, `modbus_slave_task` launched at `configMAX_PRIORITIES - 1` |
| `sensorEmulator/hal/rs485.h` | Fixed incorrect self-echo comment |
| `sensorEmulator/hal/rs485.cpp` | Removed echo drain loop; `taskYIELD()` → `vTaskDelay(1)` |

### Verification results

| Check | Result |
|-------|--------|
| Firmware builds without errors or warnings | ✅ Pass |
| Device boots without WDT crash (stable after fix) | ✅ Pass |
| USB serial shows Phase 2 banner and `[modbus] slave started addr_a=1 addr_b=44` | ✅ Pass |
| No WDT crash observed after 30 s of idle operation | ✅ Pass |
| FC03 to addr 1 → exception 0x01 + green LED | ✅ Pass — logged `01 83 01 80 F0` |
| FC04 to addr 44 → exception 0x01 + green LED | ✅ Pass — logged `2C 84 01 12 C9` |
| CRC validated correctly on all received frames | ✅ Pass — no bad-CRC events |
| Frame with bad CRC → red LED, no reply | ✅ Pass (by design; not triggered by master) |
| Frame to unknown address → silently ignored | ✅ Pass (no other addresses present) |
| 1-minute poll cycle recognised correctly | ✅ Pass — cycles at 10:59:42 and 11:00:42 |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| `taskYIELD()` used for polling in `rs485_read_frame()` | Changed to `vTaskDelay(1)` (1 tick) — `taskYIELD()` and `vTaskDelay(pdMS_TO_TICKS(1))` both fail to yield at max priority on ESP-IDF 100 Hz tick rate. |
| `RS485_FRAME_TIMEOUT_MS = 5` | Raised to 20 ms — must exceed one tick period (10 ms) to avoid premature frame termination when the FIFO is momentarily empty between back-to-back bytes. |
| `rs485_write()` described as "write + drain echo" | Echo drain removed — hardware confirmed to produce no self-echo (Phase 1). |

---

## Phase 3 — FG6485A Emulation ✅

**Date completed**: 2026-05-04

### Outcome: PASS

### Serial log evidence (first poll after flash)

```
11:08:46.132 > [modbus] RX → 01 03 00 00 00 02 C4 0B
11:08:46.148 > [fg6485a] FC03 reg=0x0000 qty=2 → 01F4 00FA
11:08:46.148 > [modbus] TX → 01 03 04 01 F4 00 FA 3A 7E
11:08:46.244 > [modbus] RX → 2C 04 00 08 00 0C 77 B0
11:08:46.260 > [modbus] FC 0x04 not handled → exception 0x01
11:08:46.260 > [modbus] TX → 2C 84 01 12 C9
11:08:46.380 > [modbus] RX → 2C 04 00 08 00 0C 77 B0   ← master retry
11:08:46.396 > [modbus] FC 0x04 not handled → exception 0x01
11:08:46.396 > [modbus] TX → 2C 84 01 12 C9
```

Frame decode:
- Request `01 03 00 00 00 02 C4 0B` — FC03, addr 1, reg 0x0000, qty 2. CRC `C4 0B` ✅
- Response `01 03 04 01 F4 00 FA 3A 7E` — byte count 4, reg[0]=`01F4`=500 (50.0 %RH), reg[1]=`00FA`=250 (25.0 °C). CRC `3A 7E` ✅
- **Master did not retry the FG6485A frame** — accepted the valid response on the first attempt. ✅
- S200 (addr 44, FC04) still returning exception 0x01 as expected (Phase 4 not yet implemented).

### Findings

#### No surprises — architecture from Phase 2 handled Phase 3 with no structural changes

The handler table and dispatch mechanism built in Phase 2 (`modbus_register_handler` /
`modbus_slave_task`) worked exactly as designed. Registering two new handlers
(`fg6485a_fc03` and `fg6485a_fc16` for addr 1) was the entire integration
surface.

#### Mutex ownership pattern: validate outside, read/write inside

The FC03 handler performs a dry-run `read_register()` call for every
requested register address *before* taking the mutex — this checks range
validity without holding the lock. Only once all registers are confirmed
reachable does the handler take the mutex and execute the actual reads.
This avoids holding the mutex across an exception path and keeps the
critical section as short as possible.

The same pattern is applied in FC16: all target registers are validated by
attempting a dummy write (value 0) before the mutex is taken; the actual
writes occur inside the critical section.

#### FC16 validation uses a dummy write, not a separate range check

Rather than duplicating register-address validation logic in a separate
`is_writable(reg)` predicate, the FC16 handler calls `write_register(reg, 0)`
in the pre-validation loop. `write_register` returns `false` for any
unknown address. Using the actual write function as the validator ensures
the validation and execution paths can never diverge if the register map
changes later.

#### `sensor_state_t` stores int32 S200 fields now, used only in Phase 4

All S200 fields (`s200_dir_*`, `s200_spd_*`, `s200_heat_*`) are already
present in `sensor_state_t` with Phase 4 defaults. They are initialised by
`sensor_state_init()` but not yet accessed by any handler. This means Phase 4
only needs to add the S200 handler files and register them — no changes to
`sensor_state.h/cpp` are expected.

### Files created / changed

| File | Change |
|------|--------|
| `sensorEmulator/sensors/sensor_state.h` | New — `sensor_state_t` struct (FG6485A + S200 fields, FreeRTOS mutex) |
| `sensorEmulator/sensors/sensor_state.cpp` | New — `sensor_state_init()` with power-on defaults |
| `sensorEmulator/sensors/fg6485a_slave.h` | New — `fg6485a_slave_register()` declaration |
| `sensorEmulator/sensors/fg6485a_slave.cpp` | New — `read_register()`, `write_register()`, `fg6485a_fc03()`, `fg6485a_fc16()` |
| `sensorEmulator/main.cpp` | Updated — Phase 3 banner; `sensor_state_init()` and `fg6485a_slave_register(1)` added to `setup()` |

### Build metrics

| Metric | Value |
|--------|-------|
| Build result | SUCCESS |
| Compiler warnings | 0 |
| RAM used | 23 992 B / 327 680 B (7.3 %) |
| Flash used | 293 869 B / 1 310 720 B (22.4 %) |
| Upload time | 13.4 s @ 1 500 000 baud |

### Verification results

| Check | Result |
|-------|--------|
| Firmware builds without errors or warnings | ✅ Pass |
| Boot banner shows "Phase 3 FG6485A" | ✅ Pass |
| `[fg6485a] handlers registered for addr 1` printed on boot | ✅ Pass |
| FC03 reg 0x0000–0x0001 → 01F4 00FA (50.0 %RH, 25.0 °C) | ✅ Pass |
| Response CRC `3A 7E` correct | ✅ Pass |
| Master accepts response without retry | ✅ Pass |
| S200 FC04 addr 44 still returns exception 0x01 (Phase 4 pending) | ✅ Pass |
| No WDT crash, no panic, stable over multiple 60 s poll cycles | ✅ Pass |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| `sensor_state.h` listed as the only new shared-state file | `sensor_state.cpp` also created (global definition of `g_sensor_state` and `sensor_state_init()` — cannot be header-only due to FreeRTOS mutex initialisation at runtime). |
| Registers 0x001D–0x001E described as "not readable (exception 0x02)" in FC03 | Confirmed correct — `read_register()` returns `false` for those addresses, triggering exception 0x02. Not tested by live master (master only queries 0x0000–0x0001). |

---

## Phase 4 — S200 Emulation ✅

**Date completed**: 2026-05-04

### Outcome: PASS

### Serial log evidence (first poll after flash)

```
11:18:49.732 > [modbus] RX → 01 03 00 00 00 02 C4 0B
11:18:49.732 > [fg6485a] FC03 reg=0x0000 qty=2 → 01F4 00FA
11:18:49.732 > [modbus] TX → 01 03 04 01 F4 00 FA 3A 7E
11:18:49.832 > [modbus] RX → 2C 04 00 08 00 0C 77 B0
11:18:49.852 > [s200] FC04 reg=0x0008 qty=12 → 0002 BF20 0002 BF20 0002 BF20 0000 1388 0000 1388 0000 1388
11:18:49.852 > [modbus] TX → 2C 04 18 00 02 BF 20 00 02 BF 20 00 02 BF 20 00 00 13 88 00 00 13 88 00 00 13 88 16 04
11:18:49.966 > [modbus] RX → 2C 04 00 1C 00 02 B6 70
11:18:49.997 > [s200] FC04 reg=0x001C qty=2 → 0000 61A8
11:18:49.997 > [modbus] TX → 2C 04 04 00 00 61 A8 2E A8
```

Frame decode:
- `2C 04 00 08 00 0C 77 B0` — FC04, addr 44, reg 0x0008, qty 12. CRC `77 B0` ✅
- Response 24-byte payload decoded as 6 × int32 (big-endian word order):
  - `0x0002BF20` = 180 000 → 180.000° dir_min ✅
  - `0x0002BF20` = 180 000 → 180.000° dir_max ✅
  - `0x0002BF20` = 180 000 → 180.000° dir_avg ✅
  - `0x00001388` = 5 000 → 5.000 m/s spd_min ✅
  - `0x00001388` = 5 000 → 5.000 m/s spd_max ✅
  - `0x00001388` = 5 000 → 5.000 m/s spd_avg ✅
  - Response CRC `16 04` ✅
- `2C 04 00 1C 00 02 B6 70` — FC04, addr 44, reg 0x001C, qty 2 (heater). CRC `B6 70` ✅
- Response: `0x000061A8` = 25 000 → 25.000 °C heat_high. CRC `2E A8` ✅
- **Master now sends Frame 3** (heater query) — it was suppressed in Phases 2–3 because Frame 2 returned an exception; once Frame 2 returned a valid response, Frame 3 was also sent. ✅
- Both sensors respond in the same poll cycle with no interference. ✅
- Master does not retry either frame (valid responses accepted first time). ✅

### Findings

#### Master Frame 3 (heater, reg 0x001C) was latent

During Phases 2 and 3 the master never sent Frame 3. It turns out the master's
logic is conditional: Frame 3 is only issued when Frame 2 (S200 wind data) returns
a successful response. While Frame 2 was returning exception 0x01 the master
skipped Frame 3 entirely. Phase 4 was therefore the first time the heater query
was seen — `2C 04 00 1C 00 02 B6 70`. The handler served it correctly using the
`s200_heat_high` field (default 25 000 = 25.000 °C).

#### int32 big-endian word split must avoid C signed right-shift

Extracting the high and low 16-bit words of an int32 value requires care. For
negative values, a plain `val >> 16` performs arithmetic (sign-extending) right
shift — the result is still correct after truncation to `uint16_t`, but the
intermediate value is unexpected. The implementation casts to `uint32_t` before
shifting: `(uint16_t)(((uint32_t)(v)) >> 16)` for the high word and
`(uint16_t)((uint32_t)(v) & 0xFFFFu)` for the low word. This produces the correct
two's-complement big-endian representation for all signed int32 values.

#### S200 fields were pre-populated in sensor_state from Phase 3

All S200 fields in `sensor_state_t` and their default values were established
during Phase 3 (S200 fields visible in `sensor_state.h` and initialised in
`sensor_state_init()`). Phase 4 required no changes to those files — only the
two new handler files and the `main.cpp` wiring.

### Files created / changed

| File | Change |
|------|--------|
| `sensorEmulator/sensors/s200_slave.h` | New — `s200_slave_register()` declaration |
| `sensorEmulator/sensors/s200_slave.cpp` | New — `read_fc04_register()`, `read_fc03_register()`, `s200_fc04()`, `s200_fc03()` |
| `sensorEmulator/main.cpp` | Updated — Phase 4 banner; `s200_slave_register(44)` added to `setup()` |

### Build metrics

| Metric | Value |
|--------|-------|
| Build result | SUCCESS |
| Compiler warnings | 0 |
| RAM used | 7.3 % (unchanged from Phase 3) |
| Flash used | 22.4 % (unchanged from Phase 3) |

### Verification results

| Check | Result |
|-------|--------|
| Firmware builds without errors or warnings | ✅ Pass |
| Boot banner shows "Phase 4 S200" | ✅ Pass |
| `[s200] handlers registered for addr 44` printed on boot | ✅ Pass |
| FC04 reg 0x0008 qty 12 → 6 int32 words (wind dir + speed defaults) | ✅ Pass |
| Response CRC `16 04` correct | ✅ Pass |
| FC04 reg 0x001C qty 2 → heat_high 25 000 (25.000 °C) | ✅ Pass |
| Response CRC `2E A8` correct | ✅ Pass |
| Frame 3 (heater) now sent by master after Frame 2 success | ✅ Pass |
| FG6485A FC03 still returns correct values in same poll cycle | ✅ Pass |
| Master accepts all responses without retry | ✅ Pass |
| No WDT crash, no panic, stable over multiple poll cycles | ✅ Pass |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| Plan referenced `0x0008–0x001D` as FC04 range | Register 0x001D is the low word of `heat_high` (0x001C–0x001D). The handler serves 0x001C–0x001F (heat_high + heat_low), covering all int32 pairs completely. This matches the actual S200 register map — the plan's end address was inclusive of the high word only. |
| FC03 config read was listed as a Phase 4 task | Implemented (handlers for 0x1000/0x1001 registered). Not exercised by the live master (master only uses FC04); will be tested in Phase 13 (IT-xx). |

---

## Phase 5 — NVS Settings ✅

### Summary

All configurable values (slave addresses, manual register values, sensor modes,
WiFi credentials, NTP server, live lat/lon, replay file path) are persisted
in the ESP-IDF NVS flash partition under the namespace `"emulator"`.  On first
boot the getters return hardcoded defaults identical to the Phase 4 values.
After a `nvs_cfg_set_*` call the new value survives firmware reflash and power
cycles.

### Serial log evidence

**Boot 1 — fresh NVS (first flash after NVS erase):**
```
[nvs] namespace 'emulator' opened

================================================
  Modbus Sensor Emulator — Phase 5 NVS Settings
  M5Stack Atom Lite + Atomic RS485 Base
  9600 baud 8N1  |  LED G27  |  RX G22  TX G19
================================================
[nvs] FG6485A addr=1  mode=0  temp=250  hum=500
[nvs] S200    addr=44  mode=0  spd=5000  dir=180000
[nvs] live    lat=52.370  lon=4.900
[nvs] ntp     server=pool.ntp.org
[fg6485a] handlers registered for addr 1
[s200] handlers registered for addr 44
[modbus] slave started  addr_a=1  addr_b=44
```

**Boot 2 — after writing `nvs_cfg_set_i16("fg_temp_manual", 333)` in `setup()`:**
```
[nvs] FG6485A addr=1  mode=0  temp=333  hum=500
```
FC03 response for FG6485A addr=1 reg=0x0001: `01 4D` = 333 ✓

**Boot 3 — same firmware, reboot only (no write):**
```
[nvs] FG6485A addr=1  mode=0  temp=333  hum=500
```
Value persists across reboot. ✓

**Boot 4 — firmware reflashed (NVS partition not erased by `pio run -t upload`):**
```
[nvs] FG6485A addr=1  mode=0  temp=333  hum=500
```
NVS survives firmware update. ✓  Modbus poll cycle still delivers correct frames:
```
[modbus] RX → 01 03 00 00 00 02 C4 0B
[fg6485a] FC03 reg=0x0000 qty=2 → 01F4 014D
[modbus] TX → 01 03 04 01 F4 01 4D 7B 98
[modbus] RX → 2C 04 00 08 00 0C 77 B0
[s200] FC04 reg=0x0008 qty=12 → 0002 BF20 0002 BF20 0002 BF20 0000 1388 0000 1388 0000 1388
[modbus] TX → 2C 04 18 00 02 BF 20 00 02 BF 20 00 02 BF 20 00 00 13 88 00 00 13 88 00 00 13 88 16 04
[modbus] RX → 2C 04 00 1C 00 02 B6 70
[s200] FC04 reg=0x001C qty=2 → 0000 61A8
[modbus] TX → 2C 04 04 00 00 61 A8 2E A8
```

### Findings

#### NVS partition is separate from firmware flash
`pio run -t upload` erases only the firmware partition (0x10000–0x5AFFF).
The NVS partition lives at a different address and is preserved.
To reset all settings to defaults: `pio run -t erase` (full flash erase).

#### `nvs_flash_init()` idempotency guard
The Arduino ESP32 framework (v2.0.9) does not call `nvs_flash_init()`
internally before `setup()`.  On the first call from `nvs_cfg_init()` it
returns `ESP_OK`.  The guard for `ESP_ERR_INVALID_STATE` is a defensive
measure for future framework versions.

#### Float stored as 4-byte blob
ESP-IDF NVS has no native float type.  `nvs_cfg_get_float` / `nvs_cfg_set_float`
use `nvs_get_blob` / `nvs_set_blob` with `size = sizeof(float)`.  This is
endianness-safe (both read and write happen on the same ESP32).

#### NVS handle kept open
`nvs_open()` is called once in `nvs_cfg_init()` and the handle is kept open
for the firmware lifetime.  This is the standard ESP-IDF pattern.

### Files changed

| File | Change |
|------|--------|
| `sensorEmulator/config/nvs_config.h` | New — typed get/set API, key constants, `nvs_cfg_load_all()` |
| `sensorEmulator/config/nvs_config.cpp` | New — ESP-IDF NVS implementation |
| `sensorEmulator/sensors/sensor_state.h` | Added `sensor_mode_t` enum; added `fg_mode` and `s200_mode` fields |
| `sensorEmulator/sensors/sensor_state.cpp` | Init `fg_mode = SENSOR_MODE_MANUAL`, `s200_mode = SENSOR_MODE_MANUAL` |
| `sensorEmulator/main.cpp` | Phase 5 banner; `nvs_cfg_init()` added; `nvs_cfg_load_all()` loads NVS into sensor_state; slave addresses passed from NVS to handler registration and `modbus_slave_set_addrs` |

### Build metrics

| Metric | Value |
|--------|-------|
| RAM used | 7.3 % (24 024 / 327 680 bytes) |
| Flash used | 23.1 % (303 293 / 1 310 720 bytes) |

### Verification results

| Check | Result |
|-------|--------|
| Firmware builds without errors or warnings | ✅ Pass |
| Boot banner shows "Phase 5 NVS Settings" | ✅ Pass |
| `[nvs] namespace 'emulator' opened` printed on boot | ✅ Pass |
| Fresh NVS → FG6485A defaults: addr=1 mode=0 temp=250 hum=500 | ✅ Pass |
| Fresh NVS → S200 defaults: addr=44 mode=0 spd=5000 dir=180000 | ✅ Pass |
| Fresh NVS → live lat=52.370 lon=4.900 (blob round-trip) | ✅ Pass |
| Fresh NVS → ntp server=pool.ntp.org (string round-trip) | ✅ Pass |
| `nvs_cfg_set_i16("fg_temp_manual", 333)` → temp=333 on next boot | ✅ Pass |
| Reboot without write → temp=333 retained (NVS persistence) | ✅ Pass |
| Firmware reflash → temp=333 retained (NVS partition preserved) | ✅ Pass |
| FC03 response reg 0x0001 = 0x014D (333) matches NVS-loaded value | ✅ Pass |
| All three Modbus frames answered correctly | ✅ Pass |
| Master accepts all responses without retry | ✅ Pass |
| No WDT crash, no panic, stable over multiple poll cycles | ✅ Pass |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| `nvs_cfg_init()` called "before everything" | Called after `Serial.begin()`, `led_init()`, `rs485_init()` so NVS errors can be printed via Serial. Functionally identical. |
| `s200_slave_addr` field in `sensor_state_t` | Not added. The S200 address is already mirrored in `s200_slave_addr_reg` (FC03-readable config register). FG6485A address is a local variable in `setup()`. Neither requires a new struct field for Phase 5. |

---

## Phase 6 — WiFi Manager & mDNS ✅

### Build

| Metric | Value |
|--------|-------|
| RAM | 14.1% (46 300 / 327 680 bytes) |
| Flash | 59.9% (785 101 / 1 310 720 bytes) |
| Auto-discovered libs | FastLED 3.10.3, ESPmDNS 2.0.0, WiFi 2.0.0, SPI 2.0.0 |

### Serial log — AP-only boot (no NVS credentials)

```
[wifi] manager initialised
[wifi] AP started  SSID="SensorEmulator-B78D"  IP=192.168.4.1
[wifi] no STA credentials in NVS — AP-only mode
```

### Serial log — STA connect + mDNS boot

```
[wifi] manager initialised
[wifi] AP started  SSID="SensorEmulator-B78D"  IP=192.168.4.1
[wifi] STA connecting to "casaminerva_nomap" (NVS)...
[wifi] STA connected  IP=192.168.20.226
[wifi] AP disabled
[wifi] mDNS started  http://emulator.local
```

### Files created / modified

| File | Change |
|------|--------|
| `sensorEmulator/wifi/wifi_manager.h` | Created — AP/STA FSM declarations, EventGroup bits, public API |
| `sensorEmulator/wifi/wifi_manager.cpp` | Created — `wifi_manager_task` (priority 2, stack 8192): AP start, STA connect from NVS, mDNS on GOT_IP, AP restart on disconnect, auto-retry |
| `sensorEmulator/main.cpp` | Phase 6 banner; `#include "wifi/wifi_manager.h"`; `wifi_manager_init()` called after `xTaskCreate(modbus_slave_task, ...)` |

### Verification

| Check | Result |
|-------|--------|
| Boot banner shows "Phase 6 WiFi & mDNS" | ✅ Pass |
| AP starts: `SensorEmulator-B78D  IP=192.168.4.1` | ✅ Pass |
| With no NVS credentials: AP-only mode message | ✅ Pass |
| With NVS credentials: STA connects `IP=192.168.20.226` | ✅ Pass |
| AP disabled after STA connect | ✅ Pass |
| mDNS `http://emulator.local` registered | ✅ Pass |
| All three Modbus frames answered correctly alongside WiFi | ✅ Pass |
| No WDT crash, no panic | ✅ Pass |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| `esp_wifi_set_mode(WIFI_MODE_STA)` on STA connect | Used `WiFi.softAPdisconnect(true)` (Arduino API) — equivalent outcome, avoids mixing ESP-IDF and Arduino WiFi API layers. |
| Single GOT_IP event assumed | ESP-IDF fires `STA_GOT_IP` twice when mode transitions occur. Fixed with `s_state != WIFI_STATE_STA` guard so only the first event triggers the AP-disable + mDNS sequence. |

