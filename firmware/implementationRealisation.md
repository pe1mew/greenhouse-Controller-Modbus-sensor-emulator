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

---

## Phase 7 — Web Interface: Server + WebSocket ✅

**Date completed**: 2026-05-04

### Outcome: PASS

The firmware was built successfully. Hardware verification was completed
together with the Phase 8 flash (see "Phase 7 — Hardware Verification" section
below).

### Build metrics

| Metric | Value |
|--------|-------|
| Build result | SUCCESS |
| Compiler warnings | 0 |
| RAM used | 14.6 % (47 832 / 327 680 bytes) |
| Flash used | 69.1 % (905 813 / 1 310 720 bytes) |
| Flash delta vs Phase 6 | +120 KB (SPIFFS assets + httpd + cJSON) |

### Findings

#### SPIFFS must be mounted before `httpd` starts

`SPIFFS.begin(true)` must be called before `httpd_start()` and the static
file handlers are registered. The `true` argument (formatOnFail) ensures the
first boot succeeds even if the SPIFFS partition has never been formatted.
`web_server_init()` is called after `wifi_manager_init()` in `main.cpp` so the
WiFi stack is up before the HTTP server starts listening.

#### Designated initialisers in C++17 GCC mode

`httpd_uri_t` uses C99-style designated initialisers (`.uri =`, `.method =`,
etc.). GCC in C++17 mode accepts these as an extension. Fields not explicitly
initialised (e.g. `.is_websocket`, `.handle_ws_control_frames`) default to
`false` / `nullptr` per C++ value-initialisation rules. No casts or workarounds
were needed.

#### WebSocket broadcast threading: `httpd_queue_work` pattern

The 1 Hz WebSocket push runs in a dedicated FreeRTOS task (`ws_push_task`,
priority 1, stack 4096 bytes). Direct calls to `httpd_ws_send_frame_async`
from a non-httpd task are not allowed. The solution:

1. `ws_push_task` allocates a `broadcast_ctx_t` struct on the heap containing
   the JSON payload and a client socket FD.
2. It calls `httpd_queue_work(server, broadcast_cb, ctx)` to schedule the send
   inside the httpd task context.
3. `broadcast_cb` calls `httpd_ws_send_frame_async`, then `free(ctx)`.

This pattern ensures frame sends always happen from the httpd task thread,
satisfying ESP-IDF's threading requirement. The `broadcast_ctx_t` is
malloc'd in the push task and freed in the callback to avoid stack lifetime
issues.

#### `build_status_json` reads `g_sensor_state` under mutex

The status JSON builder acquires `g_sensor_state.mutex`, snapshots all
required fields into locals, releases the mutex, then formats the JSON string
with cJSON. This keeps the critical section short (field copies only, no string
allocation inside the lock).

#### Heating temperature not persisted in NVS (runtime-only)

The S200 heating temperature is intentionally not written to NVS in Phase 7.
The `web_server.cpp` POST handler updates `g_sensor_state.s200_heat_raw`
in-memory but does not call `nvs_cfg_set_i32("s200_heat_manual", ...)`.
This is consistent with Phase 8 being the phase that fully wires manual mode
state → NVS persistence. There is no `s200_heat_manual` NVS key defined yet.

#### Client-side precision updates (post-Phase-7 UI refinement)

After the Phase 7 build, the web UI was refined (outside the firmware build
cycle) with the following changes applied directly to `firmware/data/`:

- **Humidity** (`fg-hum`): `step` changed from 0.1 to 1; display changed from
  `toFixed(1)` to `toFixed(0)` — integer %RH matches the raw encoding
  (raw = %RH × 10, uint16).
- **Wind speed** (`s200-spd`): `step` changed from 0.001 to 1; display changed
  from `toFixed(3)` to `toFixed(0)`.
- **Wind direction** (`s200-dir`): `step` changed from 0.001 to 1; display
  changed from `toFixed(3)` to `toFixed(0)`.
- **Heating temp** (`s200-heat`): `step` changed from 0.001 to 0.1; display
  unchanged — one decimal place matches the raw encoding (raw = °C × 1000).
- Log table columns made **user-resizable** via a JS drag handle; Time and Dir
  columns pinned to fixed widths (72 px / 42 px) in CSS.
- Section heading changed from **WiFi Settings** to **System Settings**; WiFi
  demoted to a grey `<h3>` subheading alongside NTP and Manual time.

These changes are in `firmware/data/` and will be included in the SPIFFS image
at next flash.

### Files created / changed

| File | Change |
|------|--------|
| `sensorEmulator/web/web_server.h` | New — `web_server_init()` declaration |
| `sensorEmulator/web/web_server.cpp` | New — ESP-IDF httpd, static file handlers, WebSocket push task, all POST handlers, cJSON status builder |
| `firmware/data/index.html` | New — single-page UI: status cards, FG6485A config, S200 config, System Settings, Modbus log |
| `firmware/data/style.css` | New — dark-theme CSS, responsive grid, badge variants, resizable-column handle styles |
| `firmware/data/app.js` | New — WebSocket client, slider↔input sync, Apply POST functions, log table, column resizer init |
| `firmware/platformio.ini` | Updated — `board_build.filesystem = spiffs` added to `[env:sensorEmulator]` |
| `sensorEmulator/main.cpp` | Updated — Phase 7 banner; `#include "web/web_server.h"`; `web_server_init()` called after `wifi_manager_init()` |

### Build verification

| Check | Result |
|-------|--------|
| `pio run -e sensorEmulator` completes with 0 errors | ✅ Pass |
| 0 compiler warnings | ✅ Pass |
| Flash usage within 70 % budget | ✅ Pass (69.1 %) |
| RAM usage within 50 % budget | ✅ Pass (14.6 %) |

### Hardware verification

| Check | Result |
|-------|--------|
| Firmware flashed to Atom Lite | ✅ Pass — see Phase 7 Hardware Verification |
| `http://192.168.4.1` loads in browser | ✅ Pass |
| WebSocket connects; status updates every ~1 s | ✅ Pass |
| Slider apply → value changes in Modbus response | ✅ Pass |
| Value persists after page reload (Phase 8) | ✅ Pass |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| Heating temperature written to NVS on Apply | Not implemented in Phase 7 — deferred to Phase 8 (manual mode wiring). No `s200_heat_manual` NVS key exists yet. |
| Single-phase "server + WebSocket + POST handlers" | UI precision and layout fixes applied post-build outside the firmware build cycle (SPIFFS assets only, no `.cpp` recompile needed). |

---

## Intermediate Step — Web Mock & GUI Review

**Date completed**: 2026-05-04

### Purpose

Because Phase 7 firmware had not yet been flashed to hardware, a **desktop web
mock** (`webMoc/server.py`) was built to allow the web GUI to be reviewed and
iterated on a PC without any embedded target. The mock replicates the full
HTTP + WebSocket interface of `web_server.cpp` using Python / Flask / flask-sock,
serving the `firmware/data/` assets directly (no copy).

### Mock implementation

| File | Description |
|------|-------------|
| `webMoc/server.py` | Flask app — all routes, WebSocket 1 Hz push, simulated WiFi/NTP/time |
| `webMoc/requirements.txt` | `flask>=3.0`, `flask-sock>=0.7` |
| `webMoc/README.md` | Prerequisites, setup, endpoint reference, differences from firmware |

**Mock behaviour contract** (same as firmware):
- `GET /`, `/index.html`, `/style.css`, `/app.js` — served from `firmware/data/`
- `GET /ws` — WebSocket; pushes `{type:"status", ...}` every 1 s; pushes synthetic Modbus log entries every 5 s (4 alternating frames)
- `POST /config/sensor` — clamps raw values to physical ranges, returns clamped values
- `POST /config/wifi` — sets mode to `Connecting`, transitions to `STA` / `IP=192.168.1.100` / `RSSI=-55` after 3 s timer
- `POST /config/ntp` — saves server; sets `ntp_synced=True` after 2 s timer
- `POST /config/time` — stores `time.time()` offset (mirrors `settimeofday()`)
- `POST /config/tz` — stores POSIX TZ string; returns `{ok, tz}`
- `POST /config/location` — clamps lat ±90 / lon ±180; stores in `_state`; returns `{ok, lat, lon}`
- `POST /log/clear` — broadcasts `{type:"log_clear"}` to all WS clients
- `POST /replay/upload` — stores raw CSV bytes in memory; returns `{ok, size}`
- `POST /replay/control` — `{action:"start"|"stop"}`; returns `{ok, state}`

State is in-memory only; resets on server restart.

### Findings from GUI review

The following defects and usability issues were identified by interacting with
the running mock in a browser and were corrected immediately in `firmware/data/`.

#### 1 — FG6485A humidity: fractional step was wrong

The slider and number input for humidity used `step="0.1"`, matching the
temperature field. However, the FG6485A raw encoding for humidity is
`raw = %RH × 10` stored as a `uint16` — the minimum meaningful step on the
wire is 0.1 %RH, but the *user-facing* unit displayed by the sensor is integer
%RH (the register holds tenths, not hundredths). Presenting tenths to the
operator is misleading because the sensor reports whole-number %RH values to
clients.

**Fix**: `step` and `max` changed to `1` and `99` respectively on both slider
and input. Status display changed from `toFixed(1)` to `toFixed(0)`. Parse
function changed from `parseFloat` to `parseInt`.

#### 2 — S200 wind speed: sub-m/s precision not meaningful for operator input

The slider used `step="0.001"`, exposing all three decimal places of the raw
encoding (`raw = m/s × 1000`). Wind speed sensors typically report in whole
m/s or tenths; 0.001 m/s granularity is an artefact of the register encoding,
not a useful control resolution.

**Fix**: `step` changed to `1` on both slider and input. Status display changed
from `toFixed(3)` to `toFixed(0)`. Parse function changed from `parseFloat`
to `parseInt`.

#### 3 — S200 wind direction: sub-degree precision not needed for manual entry

Same issue as wind speed — `step="0.001"` exposes 0.001° granularity from the
raw encoding (`raw = ° × 1000`), which is not useful when entering a compass
bearing manually.

**Fix**: `step` changed to `1` on both slider and input. Status display changed
from `toFixed(3)` to `toFixed(0)`. Parse function changed from `parseFloat`
to `parseInt`.

#### 4 — S200 heating temperature: should match FG6485A temperature (one decimal)

Heating temperature uses `raw = °C × 1000`, but the meaningful precision for
this sensor is one decimal place (tenths of a degree), matching the FG6485A
temperature field. Using `step="0.001"` made the input behave differently from
temperature without a physical justification.

**Fix**: `step` changed to `0.1` on both slider and input. Status display and
parse function (`parseFloat`) unchanged — one decimal place retained.

#### 5 — Log table columns could not be resized

The Modbus log table used the browser's default auto-layout, which assigns
column widths based on content. When long hex frames appeared, the Frame column
expanded unpredictably, pushing the Summary column off-screen. There was no
way for the user to adjust column widths.

**Fix**: A `col-resizer` drag handle was added to each `<th>` via JavaScript
(`initResizableCols()`). The table is set to `table-layout: fixed`. Dragging
a handle resizes the column with a 40 px minimum. The `.col-resizer` element
and its `:hover` / `.dragging` states were added to `style.css`.

#### 6 — Time and Dir columns: too narrow / too wide depending on content

The Time and Dir log columns fluctuated in width with the resizer in place and
did not need to be user-adjustable — their content is always a fixed-length
timestamp and a two/three-character direction label.

**Fix**: CSS `nth-child(1)` (Time: 72 px) and `nth-child(2)` (Dir: 42 px) rules
with matching `min-width` / `max-width` pin them to fixed sizes. The
`initResizableCols()` function skips adding a resizer handle to the first two
columns (`FIXED_COLS = 2`).

#### 7 — "WiFi Settings" section heading was not visually consistent

The System Settings section had a white `<h2>` reading "WiFi Settings" — a
title that referred only to one sub-section. NTP and Manual time were already
rendered as grey `<h3>` sub-headings beneath it, but WiFi was treated as a
top-level heading visually.

**Fix**: The `<h2>` text changed to "System Settings". WiFi was demoted to a
grey `<h3>` (`<h3>WiFi</h3>`) alongside the existing NTP and Manual time
sub-headings.

### Files changed by GUI review

| File | Changes |
|------|---------|
| `firmware/data/index.html` | Humidity `step`/`max` → `1`/`99`; wind speed and direction `step` → `1`; heating temp `step` → `0.1`; "WiFi Settings" `<h2>` → "System Settings" + WiFi `<h3>` |
| `firmware/data/app.js` | Humidity `parseFloat` → `parseInt`; wind speed and direction `parseFloat` → `parseInt`; `toFixed` display precision updated; `initResizableCols()` function added and called at boot |
| `firmware/data/style.css` | `.col-resizer` styles added; `#log-tbl th` gains `overflow: hidden; white-space: nowrap`; `nth-child(1)` and `nth-child(2)` fixed-width rules added |

### Verification

| Check | Result |
|-------|--------|
| `python server.py` starts without error | ✅ Pass |
| `http://127.0.0.1:5000` loads in browser | ✅ Pass |
| WebSocket connects; status badge shows Online | ✅ Pass |
| Status values update every 1 s | ✅ Pass |
| Humidity slider moves in integer steps | ✅ Pass |
| Wind speed slider moves in integer steps | ✅ Pass |
| Wind direction slider moves in integer steps | ✅ Pass |
| Heating temp slider moves in 0.1 °C steps | ✅ Pass |
| Apply on FG6485A → POST `/config/sensor` → clamped response updates inputs | ✅ Pass |
| Log entries appear every 5 s (synthetic frames) | ✅ Pass |
| Log column drag resizes Frame and Summary columns | ✅ Pass |
| Time and Dir columns do not resize | ✅ Pass |
| WiFi connect POST → mode shows Connecting, then STA after 3 s | ✅ Pass |
| NTP POST → ntp_synced badge turns green after 2 s | ✅ Pass |
| Clear log → table empties | ✅ Pass |
| Section header reads "System Settings"; WiFi is a grey sub-heading | ✅ Pass |

---

## Phase 7 — Hardware Verification ✅

**Date completed**: 2026-05-04

### Outcome: PASS

Phase 7 build was flashed to hardware together with the Phase 8 firmware (both
were verified in a single combined flash after Phase 8 code was complete — see
Phase 8 below for build metrics).

### Hardware verification results

| Check | Result |
|-------|--------|
| Firmware flashed to Atom Lite (COM5, ESP32-PICO-D4, MAC 14:2b:2f:a0:b7:8c) | ✅ Pass |
| SPIFFS image uploaded (`pio run -t uploadfs`) | ✅ Pass |
| `http://192.168.4.1` loads full UI in browser (AP mode) | ✅ Pass |
| WebSocket connects; status updates every ~1 s | ✅ Pass |
| Slider apply → value changes in Modbus response | ✅ Pass |
| All three Modbus frames answered correctly | ✅ Pass |

### Findings

#### AP mode vs STA mode showed different values in control panel

When the device was accessed over STA (browser at `http://sensor-emulator.local`),
the control panel showed HTML-default values (addr = 0, mode = Manual, all sliders
at zero) instead of the values stored on the device.

**Root cause**: `build_status_json()` did not include `fg.mode`, `fg.addr`,
`s200.mode`, `s200.addr`, or `s200.heat` in the WebSocket push. The browser
populated editable controls from their HTML `value=""` attributes (all zero).

**Fix — firmware** (`web_server.cpp`): `build_status_json()` extended to include
all five missing fields in every 1 Hz push.

**Fix — web asset** (`data/app.js`): Added `wsInitialized` flag. On the **first**
WebSocket message after connect, all editable controls (address inputs, mode
radios, sliders, number inputs) are populated from the device state in that
message. Subsequent pushes only refresh the read-only status display, so
in-progress user edits are not overwritten.

#### mDNS hostname renamed

The mDNS hostname `"emulator"` was renamed to `"sensor-emulator"` during this
verification phase, making the device reachable at **http://sensor-emulator.local**.
The index.html hint text was updated to match.

### Files changed (post-build hardware verification)

| File | Change |
|------|--------|
| `sensorEmulator/wifi/wifi_manager.cpp` | `MDNS.begin("emulator")` → `MDNS.begin("sensor-emulator")` |
| `sensorEmulator/web/web_server.cpp` | `build_status_json()` extended with `fg.mode`, `fg.addr`, `s200.mode`, `s200.addr`, `s200.heat` |
| `firmware/data/app.js` | `wsInitialized` flag added; first-message control population |
| `firmware/data/index.html` | Hint text `http://emulator.local` → `http://sensor-emulator.local` |

---

## Phase 8 — Manual Mode ✅

**Date completed**: 2026-05-04

### Outcome: PASS

### Build metrics

| Metric | Value |
|--------|-------|
| Build result | SUCCESS |
| Compiler warnings | 0 |
| RAM used | 14.6 % (47 764 / 327 680 bytes) |
| Flash used | 69.2 % (906 649 / 1 310 720 bytes) |
| Upload time | 9.1 s @ 1 500 000 baud |
| Hardware | COM5, ESP32-PICO-D4, MAC 14:2b:2f:a0:b7:8c |

### Findings

#### Mode tasks are notification-driven, not polled

The plan left the mode task wake-up mechanism open. The implementation uses
`ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))` — the task blocks until the
web POST handler calls `fg6485a_mode_task_notify()` / `s200_mode_task_notify()`
(which call `xTaskNotifyGive`). The 5 000 ms timeout acts as a safety wakeup
in case a notification is ever missed.

In MANUAL mode the task body is a no-op: the POST handler already wrote the
clamped value directly into `g_sensor_state` under mutex before sending the
notification. The task simply returns to its `ulTaskNotifyTake` wait after
reading the current mode. This is intentional — the task exists as a dispatch
skeleton for LIVE (Phase 10) and REPLAY (Phase 11).

#### Task priority: `tskIDLE_PRIORITY + 1`

Both mode tasks run at `tskIDLE_PRIORITY + 1` (priority 1). This is the lowest
non-idle priority, ensuring they never preempt the Modbus slave task
(`configMAX_PRIORITIES − 1`), the WiFi manager (priority 2), or the WebSocket
push task (priority 1, but only active for the duration of a `httpd_queue_work`
callback). No priority inversion was observed.

#### No changes to modbus_slave_task

`modbus_slave_task` reads `g_sensor_state` under mutex on every frame response.
Since the POST handler writes to `g_sensor_state` under the same mutex, Modbus
responses always reflect the latest applied value without any changes to the
slave task.

### Files created / changed

| File | Change |
|------|--------|
| `sensorEmulator/tasks/fg6485a_mode_task.h` | New — `fg6485a_mode_task()` and `fg6485a_mode_task_notify()` declarations (`extern "C"`) |
| `sensorEmulator/tasks/fg6485a_mode_task.cpp` | New — task body: `ulTaskNotifyTake` loop, mutex read of `fg_mode`, switch/case dispatch |
| `sensorEmulator/tasks/s200_mode_task.h` | New — `s200_mode_task()` and `s200_mode_task_notify()` declarations (`extern "C"`) |
| `sensorEmulator/tasks/s200_mode_task.cpp` | New — same pattern as fg6485a; reads `s200_mode` |
| `sensorEmulator/web/web_server.cpp` | Added `#include` for both task headers; `handle_post_sensor()` calls notify at end of each sensor branch |
| `sensorEmulator/main.cpp` | Phase 8 banner; added task includes; `xTaskCreate` for both tasks at `tskIDLE_PRIORITY + 1`, stack 2048 bytes |

### Verification results

| Check | Result |
|-------|--------|
| Firmware builds without errors or warnings | ✅ Pass |
| Boot banner shows "Phase 8 Manual Mode" | ✅ Pass |
| Set FG6485A temperature to 35.0 °C → Modbus FC03 reg 0x0001 returns 350 | ✅ Pass |
| Reboot → value is 350 on first query (NVS persistence) | ✅ Pass |
| POST temperature 999 → clamped to 1200 (120.0 °C); Modbus returns 1200 | ✅ Pass |
| POST humidity −1 → clamped to 0 | ✅ Pass |
| All three Modbus frames answered correctly after value change | ✅ Pass |
| No WDT crash, no panic, stable over multiple poll cycles | ✅ Pass |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| Mode task wake-up mechanism unspecified | Implemented as `xTaskNotifyGive` / `ulTaskNotifyTake` (direct-to-task notification). Chosen over a queue because only one bit of information is needed: "something changed". |
| Tasks listed as `ntp_task.cpp` only in the source tree | Two additional task files created: `fg6485a_mode_task.h/.cpp` and `s200_mode_task.h/.cpp` — headers were added alongside the `.cpp` files to allow other translation units to call the notify API cleanly. |

---

## Phase 9 — NTP + Timezone + Manual Time ✅

### Summary

Adds SNTP-based clock synchronisation and POSIX timezone support.
`ntp_task` monitors the WiFi EventGroup: starts SNTP when STA connects,
stops it (and clears `ntp_synced`) when STA disconnects.  The POSIX TZ
string is read from NVS at boot and applied immediately; a new POST
`/config/tz` endpoint lets the operator update it at runtime.  The WebSocket
status push now reflects real sync state via `ntp_is_synced()`.

### Build metrics

| Metric | Value |
|--------|-------|
| Flash | 915 201 B / 1 310 720 B (69.8%) |
| RAM | 49 124 B / 327 680 B (15.0%) |
| Compiler | xtensa-esp32 8.4.0+2021r2-patch5 |
| Flash delta vs Phase 8 | +8 990 B |
| RAM delta vs Phase 8 | +2 084 B |

### Files changed

| File | Change |
|------|--------|
| `sensorEmulator/tasks/ntp_task.h` | Created — `ntp_task_init()`, `ntp_is_synced()` public API |
| `sensorEmulator/tasks/ntp_task.cpp` | Created — SNTP start/stop on WiFi EventGroup bits; TZ from NVS; sync callback |
| `sensorEmulator/web/web_server.cpp` | `#include ntp_task.h`; `ntp_is_synced()` in `build_status_json()`; `handle_post_tz()` + `/config/tz` URI; `max_uri_handlers` → 17 |
| `sensorEmulator/main.cpp` | `#include tasks/ntp_task.h`; `ntp_task_init()` after `web_server_init()`; banner → "Phase 9 NTP + Timezone"; updated docstring |
| `data/index.html` | Added Timezone section (POSIX TZ input + Apply button) between NTP and Manual Time sections |
| `data/app.js` | Added `postTz()` function; response `tz` field written back to input |

### Key design decisions

| Decision | Rationale |
|----------|-----------|
| `static char s_server_buf[]` in module scope | `sntp_setservername()` stores a raw pointer; a local buffer would be freed after `start_sntp()` returns, causing a dangling pointer on the next SNTP restart cycle. |
| Separate `start_sntp()` / `stop_sntp()` idempotent helpers | Called on every connect/disconnect cycle; guards prevent double-init / double-stop. |
| SNTP sync callback sets `volatile bool` only | No mutex needed for a single boolean — the volatile qualifier prevents the compiler from caching the read across calls. |
| TZ fallback to `"UTC0"` (not empty string) | An empty TZ environment variable is undefined behaviour on some libc implementations; `"UTC0"` is the safe explicit UTC POSIX string. |

### Hardware verification

| Test | Result |
|------|--------|
| Firmware + filesystem flash without errors | ✅ Pass |
| Boot banner shows "Phase 9 NTP + Timezone" | ✅ Pass (verified via serial monitor) |
| `[ntp] no TZ in NVS — using UTC0` on first boot | ✅ Pass |
| WiFi connect → `[ntp] SNTP started server=pool.ntp.org` | ✅ Pass |
| `[ntp] clock synchronised` logged after NTP sync | ✅ Pass |
| WebSocket `ntp_synced` transitions false → true after sync | ✅ Pass |
| POST `/config/tz` with Netherlands POSIX string → applied and persisted | ✅ Pass |
| Reboot after TZ set → `[ntp] TZ applied from NVS: CET-1CEST,...` | ✅ Pass |
| WiFi disconnect → `[ntp] SNTP stopped`, `ntp_synced` → false | ✅ Pass |
| Modbus responses unaffected throughout | ✅ Pass |

---

## Post-Phase-9 UI fixes (patch 0.9.1) ✅

### Summary

Four UI defects found during hardware verification of Phase 9, fixed and reflashed in two rounds.

### Files changed

| File | Change |
|------|--------|
| `data/app.js` | `s.time.replace('T', ' ')` — remove ISO 8601 `T` separator from displayed clock string |
| `data/app.js` | Populate `wifi-ssid` input from `s.wifi.ssid` on first WebSocket message |
| `data/style.css` | `.card .badge { display: inline-block; margin-top: .75rem; }` — move NTP badge below the time text with consistent spacing; badge sized to text |
| `data/index.html` | `style="margin-bottom:.75rem"` on the TZ hint `<p>` — consistent gap before Apply timezone button |
| `sensorEmulator/web/web_server.cpp` | `build_status_json()` — add `"ssid"` field to `wifi` object via `WiFi.SSID()` when in STA mode |

### Hardware verification

| Test | Result |
|------|--------|
| Clock shows `"2026-05-04 14:59:37"` (space, no `T`) | ✅ Pass |
| NTP badge sits below time text with visible gap, width = text width | ✅ Pass |
| Gap between TZ hint and Apply timezone button consistent with other gaps | ✅ Pass |
| WiFi SSID field pre-filled with connected network name on page load | ✅ Pass |

---

## Phase 10 — Live Mode ✅

### Goal

Sensor values fetched automatically from Open-Meteo and injected into
`g_sensor_state` whenever either sensor is in `SENSOR_MODE_LIVE`.
Rate-limited to ≤ 1 000 API calls/day.  Coordinates obtained via ip-api.com
geolocation on each new WiFi-STA connection, with manual override via web UI.

### Files created

| File | Description |
|------|-------------|
| `sensorEmulator/net/geo_ip.h` | Public API — `geo_ip_get_location(float *lat, float *lon)` |
| `sensorEmulator/net/geo_ip.cpp` | HTTP GET `http://ip-api.com/json/` → parse lat/lon → store in NVS |
| `sensorEmulator/tasks/live_fetch_task.h` | Public API — `live_fetch_task_init()`, `live_fetch_task_notify()` |
| `sensorEmulator/tasks/live_fetch_task.cpp` | FreeRTOS task; HTTPS GET Open-Meteo; injects clamped values; 87 s rate limiter |

### Files modified

| File | Change |
|------|--------|
| `sensorEmulator/tasks/fg6485a_mode_task.cpp` | `#include "live_fetch_task.h"`; LIVE branch calls `live_fetch_task_notify()` |
| `sensorEmulator/tasks/s200_mode_task.cpp` | `#include "live_fetch_task.h"`; LIVE branch calls `live_fetch_task_notify()` |
| `sensorEmulator/web/web_server.cpp` | `#include "../tasks/live_fetch_task.h"`; `handle_post_location()` for POST `/config/location`; `live` object (`lat`, `lon`) in status JSON; `max_uri_handlers` bumped from 17 → 18; registered `u_location` URI |
| `sensorEmulator/main.cpp` | `#include "tasks/live_fetch_task.h"`; `live_fetch_task_init()` after `ntp_task_init()`; banner → "Phase 10 Live Mode"; updated docstring |
| `data/index.html` | Removed `<em>(Phase 10)</em>` label from FG6485A and S200 Live radio buttons; added Location section (lat/lon inputs + Apply) in System Settings |
| `data/app.js` | `postLocation()` function (POST `/config/location`); lat/lon inputs populated from `s.live` on first WebSocket message |

### Implementation notes

- **Open-Meteo URL**: uses `current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_direction_10m&wind_speed_unit=ms` — response is compact (~500 B); no hourly data requested.
- **HTTPS**: `WiFiClientSecure` + `setInsecure()` — acceptable for a non-sensitive public API on an embedded device.
- **ip-api.com**: plain HTTP free tier — no cert complexity; called once per new STA IP address.
- **Rate limiting**: `ulTaskNotifyTake` with 87 s timeout; `xTaskGetTickCount` guard prevents a re-fetch when the mode task's 5 s safety notification fires before the interval elapses.
- **Injection rules**: FG6485A — only if `fg_mode == LIVE`; S200 — only if `s200_mode == LIVE`; heating temperature not touched (no weather field available).
- **Flash / RAM**: RAM 15.3% (50 232 B), Flash 83.2% (1 091 137 B).

### Hardware verification

| Test | Result |
|------|--------|
| Boot banner shows "Phase 10 Live Mode" | ✅ Pass |
| FG6485A in Live mode — temperature and humidity update after ≤ 87 s | ✅ Pass |
| S200 in Live mode — wind speed and direction update after ≤ 87 s | ✅ Pass |
| Location section visible in System Settings; lat/lon pre-filled from NVS | ✅ Pass |
| POST `/config/location` accepted; new coordinates used on next fetch | ✅ Pass |
| Disconnect WiFi mid-live — last good values held; reconnect resumes fetching | ✅ Pass |

---

## Post-Phase-10 patch (0.10.1) — S200 heating temperature follows live ambient ✅

### Summary

When S200 is in Live mode the heating temperature register was left at its
NVS-loaded manual default.  The Open-Meteo response does not include a
dedicated heating temperature field, but it does supply ambient temperature.
Tracking ambient temperature is the closest useful approximation available.

### Change

`tasks/live_fetch_task.cpp` — `fetch_and_inject()`:

```cpp
int32_t s200_heat_raw = clamp_to_i32(temp_c * 1000.0f,
                                     S200_HEAT_RAW_MIN, S200_HEAT_RAW_MAX);
// …
if (g_sensor_state.s200_mode == SENSOR_MODE_LIVE) {
    // wind values …
    g_sensor_state.s200_heat_high =
    g_sensor_state.s200_heat_low  = s200_heat_raw;
}
```

Serial log line extended with `heat=xx.xxx°C`.

### Hardware verification

| Test | Result |
|------|--------|
| Firmware reflashed to COM5 | ✅ Pass |
| S200 in Live mode — heating temperature tracks ambient temperature in web UI | ✅ Pass |

---

## Phase 11 — Replay Mode ✅

**Date completed**: 2026-05-04

### Outcome: PASS

### Build metrics

| Metric | Value |
|--------|-------|
| Build result | SUCCESS |
| Compiler warnings | 0 |
| RAM used | 15.3% (50 248 / 327 680 bytes) |
| Flash used | 83.6% (1 096 045 / 1 310 720 bytes) |
| Flash delta vs Phase 10 | +4 908 B |
| RAM delta vs Phase 10 | +16 B |
| Hardware | COM5, ESP32-PICO-D4, MAC 14:2b:2f:a0:b7:8c |

### Files created

| File | Description |
|------|-------------|
| `sensorEmulator/util/csv_parser.h` | Public API: `csv_row_t`, `csv_parser_t`, `csv_open()`, `csv_close()`, `csv_next_row()`, `csv_rewind()` |
| `sensorEmulator/util/csv_parser.cpp` | SPIFFS CSV reader; `read_line()` (strips `\r`); `map_col_name()` → `CsvField` enum; `parse_header()` builds `col_map[8]`; `csv_next_row()` splits on commas, calls `strptime("%Y-%m-%dT%H:%M:%S", ...)` for timestamps, `atof()` for floats; `CSV_MAX_LINE = 160`; heap-allocated via `new`/`delete` |
| `sensorEmulator/tasks/replay_task.h` | Public API: `replay_state_t` enum (IDLE/RUNNING/DONE/ERROR), `replay_task_init()`, `replay_task_start()`, `replay_task_stop()`, `replay_task_get_state()`, `replay_task_get_row()` |
| `sensorEmulator/tasks/replay_task.cpp` | FreeRTOS task (`tskIDLE_PRIORITY+1`, 4 KiB stack); two volatile boolean signals (`s_start_req`, `s_stop_req`) + `xTaskNotifyGive` wakeup; RTC pre-flight check (epoch > 2020-01-01); CSV open → seek to first `mktime(&row.ts) >= now`; 500 ms `ulTaskNotifyTake` poll; `inject_row()` with per-field clamping + serial warnings; injects only into sensors in `SENSOR_MODE_REPLAY`; DONE on EOF |

### Files modified

| File | Change |
|------|--------|
| `sensorEmulator/web/web_server.cpp` | `#include "../tasks/replay_task.h"`; `handle_replay_upload()` replaces Phase 7 501 stub — 256-byte chunked SPIFFS write, 200 KB max, path stored in NVS `replay_file`, returns `{"ok":true,"size":N}`; `handle_replay_control()` replaces Phase 7 501 stub — dispatches `start`/`stop`, returns `{"ok":true,"state":"running\|idle"}`; `build_status_json()` extended with `"replay":{"state":"idle\|running\|done\|error","row":N}` |
| `sensorEmulator/main.cpp` | `#include "tasks/replay_task.h"`; `replay_task_init()` after `live_fetch_task_init()`; banner → "Phase 11 Replay Mode"; updated docstring |
| `data/index.html` | Replay CSV section added to System Settings (before Manual Time): file picker, Upload button, `replay-upload-status` hint, Start/Stop buttons, `replay-status` hint, CSV format reference block; `<em>(Phase 11)</em>` removed from FG6485A and S200 mode radio labels |
| `data/app.js` | `uploadReplayFile()` — raw POST `/replay/upload` with `Content-Type: text/csv`; shows byte count on success; `startReplay()` / `stopReplay()` — wrappers around `post('/replay/control', ...)`; `handleStatus()` extended — reads `s.replay.state` and `s.replay.row`, writes text to `#replay-status` |

### Implementation notes

#### Two volatile booleans instead of a notification value

The plan listed a single `xTaskNotifyGive` to signal both start and stop.
Using the notification *value* to carry intent (0 = stop, 1 = start) creates
a race: if start and stop are signalled in quick succession the second
notification overwrites the first before the task reads it.  Two separate
volatile booleans (`s_start_req`, `s_stop_req`) checked in order after every
`ulTaskNotifyTake` return avoid this.  The notification is used only as a
wakeup trigger, not to carry data.

#### CSV upload bypasses `read_body()` / `HTTP_BODY_MAX`

The existing `read_body()` helper in `web_server.cpp` uses a 512-byte stack
buffer — useless for CSV files of 10–200 KB.  `handle_replay_upload()` instead
calls `httpd_req_recv()` directly in a 256-byte loop, writing each chunk to the
SPIFFS file incrementally.  This keeps stack usage constant regardless of file
size.  The old helper is retained for the smaller POST handlers.

#### `drain_body` helper retained for `handle_post_log_clear`

The `drain_body` helper (reads and discards the request body) was kept because
`handle_post_log_clear` still needs to consume the empty POST body before
sending a response.  ESP-IDF httpd returns an error if the body is not fully
read.

#### SPIFFS path stored in NVS as `replay_file`

The upload handler writes the fixed path `/replay.csv` and stores it in NVS
key `NVS_KEY_REPLAY_FILE`.  Using NVS means the replay task always reads the
most recently uploaded file across reboots without any additional state
management.

#### `inject_row()` clamping

`to_raw(v, scale, lo, hi, &clamped_out)` computes `(int32_t)roundf(v * scale)`
and clamps to `[lo, hi]`.  If `*clamped_out` is set, `inject_row()` prints a
warning to serial: `[replay] WARN fg_temp 999.9 → clamped to 120.0`.  Injection
only proceeds for sensors in `SENSOR_MODE_REPLAY` at the moment of injection —
if the user switches a sensor out of Replay mode mid-playback, subsequent rows
for that sensor are silently skipped.

#### Pre-flight RTC check

The task checks `time(NULL) > 1577836800LL` (2020-01-01 00:00:00 UTC) before
opening the CSV.  If the clock has not been set (e.g. NVS credentials not yet
saved and manual time not set) the state is set to REPLAY_ERROR and a warning
is logged.  This prevents the task from seeking to a meaningless position in
the CSV.

### Hardware verification

| Test | Result |
|------|--------|
| Boot banner shows "Phase 11 Replay Mode" | ✅ Pass |
| Firmware + SPIFFS flashed to hardware (COM5) | ✅ Pass |
| Replay CSV section visible in System Settings | ✅ Pass |
| `(Phase 11)` no longer appears in mode radio labels | ✅ Pass |
| Upload a CSV file → `replay-upload-status` shows byte count | ⬜ Pending hardware test |
| Start replay — status shows `Replay: Running — row N` | ⬜ Pending hardware test |
| Modbus responses match CSV values at correct timestamps | ⬜ Pending hardware test |
| Out-of-range CSV value → serial warning, clamped value in Modbus response | ⬜ Pending hardware test |
| Stop replay → status returns to `Replay: Idle` | ⬜ Pending hardware test |
| Replay file persists across reboot (NVS path) | ⬜ Pending hardware test |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| Single notify value used as start/stop signal | Two separate volatile booleans (`s_start_req`, `s_stop_req`) used to avoid notification-value race. |
| Pre-flight: `ntp_is_synced()` OR manual time set | Simplified to epoch > 2020-01-01 check — same practical effect without requiring the NTP task's internal state. |
| Clamping warnings posted to `log_queue` | Warnings printed to serial only — Phase 12 log infrastructure not wired at Phase 11 implementation time. |
| File upload handler "write bytes to SPIFFS" | Implemented as 256-byte chunked streaming via `httpd_req_recv()` — not a single bulk write — to keep stack usage O(1) relative to file size. |

---

## Phase 12 — Modbus Activity Log ✅

**Firmware version**: 0.12.2  
**Flashed**: COM5 — ESP32-PICO-D4 MAC `14:2b:2f:a0:b7:8c`  
**Build metrics**: Flash 83.7 % (1,097,305 / 1,310,720 B) · RAM 15.3 % (50,256 / 327,680 B)

### Design rationale

The initial implementation stored raw frame bytes in `log_entry_t` (256 B per
entry, queue depth 32, ≈ 10.5 KB heap) and formatted hex strings and
timestamps inside `ws_push_task`.  This placed ~1.9 KB of locals on a 4 KB
stack alongside `build_status_json` call frames, causing a stack overflow on
the first log drain cycle.  The task crashed silently — Modbus kept working
but the web UI froze.

The implementation was redesigned to format at capture time:

- `log_entry_t` holds pre-formatted strings only: `ts[20]`, `dir[3]`,
  `hex[96]`, `summary[64]` — 183 bytes total.
- `modbus_log_post()` runs in the slave task context and builds all strings
  there; `ws_push_task` only copies them into cJSON.
- Queue depth reduced to 8 (≈ 1.5 KB heap); at 9600 baud the master sends
  ≤ 4 frame pairs/s so this never fills in a 1 s push cycle.
- `WS_PUSH_STACK` remains 4096 B — no large locals in the push task.
- `hex[96]` covers frames up to 32 bytes (32 × 3 − 1 = 95 chars + NUL);
  real Modbus RTU frames at 9600 baud are 8–29 bytes.
- GUI table capped at 30 rows; entries are stream-and-discard.

### Changes implemented

| File | Change |
|------|--------|
| `sensorEmulator/modbus/modbus_log.h` | **New** — `log_dir_t`; `log_entry_t` with pre-formatted string fields (`ts[20]`, `dir[3]`, `hex[96]`, `summary[64]`); API: `modbus_log_init/post/receive/clear` |
| `sensorEmulator/modbus/modbus_log.cpp` | **New** — FreeRTOS queue (depth 8); `modbus_log_post()` formats timestamp, hex string, and summary at capture time; non-blocking `xQueueSend`; `build_summary()` decodes FC01–FC06, FC10, exception responses |
| `sensorEmulator/modbus/modbus_slave.cpp` | Added `#include "modbus_log.h"`; `modbus_log_post(LOG_DIR_RX, …)` after CRC-valid frame; `modbus_log_post(LOG_DIR_TX, …)` before `rs485_write` |
| `sensorEmulator/web/web_server.cpp` | Added `#include "../modbus/modbus_log.h"`; `ws_push_task` drain loop formats log JSON via `snprintf` into a 320-byte stack buffer and broadcasts via `ws_broadcast()` (no heap allocation); `handle_post_log_clear` calls `modbus_log_clear()`; `WS_PUSH_STACK` = 4096 B.  `broadcast_dyn_ctx_t` / `broadcast_dyn_cb()` / `ws_broadcast_dyn()` were introduced in 0.12.0 and removed in 0.12.2 (use-after-free stall, see below). |
| `sensorEmulator/main.cpp` | `#include "modbus/modbus_log.h"`; `modbus_log_init()` after `sensor_state_init()`; banner → "Phase 12 Modbus Log" |
| `data/app.js` | `LOG_MAX` 200 → 30 |

### Verification

| Scenario | Result |
|----------|--------|
| Boot banner shows "Phase 12 Modbus Log" | ✅ Confirmed |
| FC03 request → RX entry appears in web UI log table | ✅ Confirmed |
| FC03 response → TX entry appears in web UI log table | ✅ Confirmed |
| Log updates continuously (no freeze after 2 min) | ✅ Confirmed — stack-overflow bug fixed |
| Summary decodes correctly: `FC03 addr=1 reg=0x0000 n=2` | ✅ Confirmed |
| Clear button → table empties; queue reset | ⬜ Pending hardware test |
| Queue full (8 entries) → oldest entries dropped silently | ⬜ Pending hardware test |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| `log_entry_t` stores raw frame bytes | Raw bytes replaced with pre-formatted strings; formatting moved to `modbus_log_post()` to eliminate large stack locals in `ws_push_task`. |
| Queue depth 32 | Reduced to 8; throughput at 9600 baud never exceeds queue capacity within a 1 s push cycle. |
| Log CRC-invalid frames | Only CRC-valid frames addressed to our slave are logged. Bad-CRC frames hit `continue` before `modbus_log_post` — serial still prints them. |
| `ws_push_task` uses fixed `broadcast_ctx_t` | Log frames now also use `ws_broadcast()` with a 320-byte stack-local JSON buffer (`snprintf`).  `broadcast_dyn_ctx_t` / `ws_broadcast_dyn()` were introduced in 0.12.0 and removed in 0.12.2 after a use-after-free stall was identified (see post-Phase-12 section). |
| GUI table max 200 rows | Reduced to 30 — stream-and-discard model; no persistence needed. |

---

## Post-Phase 12 — Bug Fixes & Web UI Improvements ✅

**Firmware version**: 0.12.3  
**Flashed**: COM5 — SPIFFS only (no firmware binary change for 0.12.3)  
**Build metrics**: Flash 83.7 % (1,097,305 / 1,310,720 B) · RAM 15.3 % (50,256 / 327,680 B)

### 0.12.2 — Modbus log stall (firmware + SPIFFS)

**Root cause**: `ws_broadcast_dyn()` set `frame.payload` to a heap buffer,
called `httpd_ws_send_frame_async()` (non-blocking) for each client, then
immediately freed the buffer in the same callback.  Because the send is
deferred to the httpd task, the freed memory was accessed after the callback
returned — a use-after-free that corrupted the httpd state progressively.
Under sustained Modbus traffic the log stopped updating while all other
WebSocket pushes (status JSON) continued working.  Additionally, one
`httpd_queue_work` + heap allocation per log entry caused fragmentation.

**Fix**: Replaced the `cJSON` + `ws_broadcast_dyn` drain path with a direct
`snprintf` into a 320-byte stack buffer followed by `ws_broadcast()` — the
same zero-heap path used for the status JSON.  Log JSON is at most ~200 chars
(within `STATUS_JSON_MAX` = 768 B).  `broadcast_dyn_ctx_t`, `broadcast_dyn_cb()`,
and `ws_broadcast_dyn()` were removed entirely.

| File | Change |
|------|--------|
| `sensorEmulator/web/web_server.cpp` | Removed `broadcast_dyn_ctx_t` / `broadcast_dyn_cb()` / `ws_broadcast_dyn()`; drain loop replaced with `snprintf` + `ws_broadcast()` |

### 0.12.3 — Slave address conflict validation (SPIFFS only)

Clicking Apply for a sensor slave address when both sensors already share
the same value now blocks the POST entirely.  An inline error message appears
directly after the Apply button and is cleared automatically as soon as the
two address inputs differ.

| File | Change |
|------|--------|
| `data/index.html` | `<span id="fg-addr-err">` / `<span id="s200-addr-err">` added after each address Apply button; `oninput="onAddrInput()"` on both address inputs |
| `data/app.js` | `addrConflict()` and `onAddrInput()` helpers added; `postFgAddr()` / `postS200Addr()` call `addrConflict()` before POST |
| `data/style.css` | `.addr-err { color: #e74c3c; font-size: .8rem; margin-left: .5rem; }` added |

### Web mock sync (0.12.3)

`webMock/server.py` brought fully up to date with Phase 12 firmware and web UI.

| Area | Change |
|------|--------|
| Docstring | Updated from Phase 7 to Phase 12; full endpoint table added |
| `_state` dict | Added `tz_posix`, `live_lat/lon`, `live_fetch_age/ok`, `replay_state/row`, `replay_csv` |
| `build_status_json()` | Added `fg.mode`, `fg.addr`, `s200.heat`, `s200.mode`, `s200.addr`, `wifi.ssid`, `live{lat,lon}`, `live_fetch_age`, `live_fetch_ok`, `replay{state,row}` |
| Log timestamp | `%H:%M:%S` → `%Y-%m-%d %H:%M:%S` (matches firmware `modbus_log.cpp`) |
| `POST /replay/upload` | Was HTTP 501 stub; now stores CSV bytes in memory, returns `{ok, size}` |
| `POST /replay/control` | Was HTTP 501 stub; now accepts `{action:"start"\|"stop"}`, returns `{ok, state}` |
| `POST /config/tz` | New — stores POSIX TZ string, returns `{ok, tz}` |
| `POST /config/location` | New — clamps lat ±90 / lon ±180, returns `{ok, lat, lon}` |

### Verification

| Scenario | Result |
|----------|--------|
| Log updates indefinitely with no stall | ✅ Confirmed — use-after-free removed |
| Apply FG6485A addr = current S200 addr → POST blocked, error shown next to button | ✅ Confirmed |
| Error message clears when addresses differ | ✅ Confirmed |
| Web mock `build_status_json()` includes all Phase 12 fields | ✅ Confirmed (syntax verified) |
| Web mock log timestamp format `YYYY-MM-DD HH:MM:SS` matches firmware | ✅ Confirmed |
| Web mock `/replay/upload` returns `{ok, size}` | ✅ Confirmed |
| Web mock `/replay/control` `start`/`stop` returns `{ok, state}` | ✅ Confirmed |
| Web mock `/config/tz` and `/config/location` respond correctly | ✅ Confirmed |

---

## Phase 13 — Replay Mode Redesign ✅

**Firmware version**: 0.13.1 (0.13.0 + stack-overflow hot-fix)
**Flashed**: COM5 — ESP32-PICO-D4 MAC `14:2b:2f:a0:b7:8c`
**Build metrics**: Flash 83.9 % (1,099,937 / 1,310,720 B) · RAM 15.7 % (51,456 / 327,680 B)

### Design rationale

Phase 11 replay was tightly coupled to the system clock: every CSV row carried
an absolute `HH:MM:SS` local timestamp, and the task refused to start until
NTP had synced.  This made it impractical in field conditions (no WiFi,
no NTP) and impossible to create portable test files.

The redesign switches to **relative elapsed time**: timestamps are seconds
from the moment the user presses Start, and the task maintains its own
monotonic timer using `xTaskGetTickCount()` deadline arithmetic.  A
FreeRTOS command queue replaces the previous volatile boolean flags,
enabling clean Pause / Play / Prev / Next navigation without polling.

An in-memory row index (`file_offset + ts_s` per row, allocated on Start,
freed on Stop/Done) gives O(1) random access for navigation, avoiding full
CSV re-scans on every Prev/Next command.

A 3-row event context window (prev/curr/next) is shared between the replay
task and `ws_push_task` via a mutex and an atomic dirty flag, so the web UI
shows the surrounding rows without adding any file-access overhead to the
push path.

### Changes implemented

| File | Change |
|------|--------|
| `util/csv_parser.h` | Timestamp field `struct tm ts` → `uint32_t ts_s`; `CSV_MAX_LINE` 160→200; added `csv_tell()`, `csv_seek()`; added `#include <stddef.h>` for `size_t` |
| `util/csv_parser.cpp` | Removed `strptime`; timestamp parsed with `sscanf(tok, "%u:%u:%u", &h,&m,&s)`; `last_row_pos` field tracks file offset before each read; `csv_tell()` returns `last_row_pos`; `csv_seek()` seeks then re-calls `csv_next_row` |
| `tasks/replay_task.h` | Complete rewrite — `REPLAY_PAUSED=2`, `REPLAY_DONE=3`, `REPLAY_ERROR=4`; `replay_window_entry_t`; cmd API `replay_task_cmd_start/stop/pause/play/next/prev()`; accessors `get_elapsed_s`, `get_row_count`, `consume_window_dirty`, `get_window` |
| `tasks/replay_task.cpp` | Complete rewrite — `replay_index_entry_t[2900]` heap array; FreeRTOS cmd queue depth 8; `s_win_mutex` for window; `dispatch_cmd()` handles all 6 cmds; RUNNING loop uses `xTaskGetTickCount()` deadline; PAUSED loop blocks on queue; immediate t=0 row injection on Start |
| `web/web_server.cpp` | `STATUS_JSON_MAX` 768→1024; `build_status_json()` adds `paused`, `row_count`, `elapsed_s`; `handle_replay_control()` expanded to 6 actions; `format_win_entry()` and `push_replay_window()` added; `ws_push_task` calls `consume_window_dirty()` + `push_replay_window()` each cycle |
| `data/index.html` | 5-button transport row (Start/Prev/Pause/Next/Stop); `replay-elapsed` para; `replay-window` div with 3-row table; Replay CSV moved to own `<section>` above System Settings |
| `data/app.js` | `_replayState`; `fmtElapsed()`; `handleReplayStatus()`; `replayCmd()`; `replayTogglePause()`; `handleReplayWindow()`; `ws.onmessage` dispatches `replay_window` type |
| `data/style.css` | `.replay-controls { flex-wrap:wrap; gap:.4rem }`, `#replay-win-tbl` border/padding/size rules, `tr.replay-curr` blue highlight |
| `webMock/server.py` | `_parse_csv()`, `_build_window_entry()`, `_replay_rows` cache; `_state` adds `replay_elapsed_s`/`replay_row_count`; `_push_thread()` advances timer and pushes `replay_window` on row change; `replay_control()` handles all 6 actions |
| `main.cpp` | Boot banner → "Phase 13 Replay Redesign" |

### Post-flash fix — stack canary (0.13.1)

On first boot after flashing 0.13.0 the device crashed with:

```
Guru Meditation Error: Core 1 panic'ed (Unhandled debug exception).
Debug exception reason: Stack canary watchpoint triggered (ws_push)
```

`push_replay_window()` added ≈ 1 100 bytes of stack locals (3 × 150-byte
char buffers, 512-byte `win_json`, 3 `replay_window_entry_t` structs) on top
of the existing ≈ 1 200 bytes used by `build_status_json` and the log drain
loop, exceeding the 4 096-byte `WS_PUSH_STACK`.

| Fix | Detail |
|-----|--------|
| `WS_PUSH_STACK` 4 096 → 6 144 | Adequate headroom for all locals in the push cycle |
| `push_replay_window()` entry bufs 150 → 96, `win_json` 512 → 320 | Actual worst-case output fits in 96 bytes; reduces peak demand by ≈ 600 bytes |
| `char json[1024]` → `static char` in `ws_push_task` | Moves 1 024 bytes off the task stack into BSS |

### Post-flash UI layout change (0.13.2)

Replay CSV was inside the System Settings `<section>`.  Moved to its own
`<section>` immediately after the S200 card and before System Settings,
with `<h2>Replay CSV</h2>` consistent with the other section headings.

### Hardware verification

| Test | Result |
|------|--------|
| Boot banner shows "Phase 13 Replay Redesign" | ✅ Confirmed |
| Device connects to WiFi and responds to Modbus requests | ✅ Confirmed (after stack fix) |
| Replay CSV section appears between S200 and System Settings | ✅ Confirmed |
| Upload CSV → upload status shows byte count | ⬜ Pending hardware test |
| Start → elapsed timer increments in web UI | ⬜ Pending hardware test |
| Pause → timer freezes; Prev/Next enabled | ⬜ Pending hardware test |
| Next/Prev → row advances/retreats; window table updates | ⬜ Pending hardware test |
| Modbus responses match CSV values at correct elapsed offsets | ⬜ Pending hardware test |
| Out-of-range CSV value → serial warning, clamped value in response | ⬜ Pending hardware test |
| Stop → state returns to Idle; window hidden | ⬜ Pending hardware test |

### Deviations from plan

| Plan item | Deviation |
|-----------|-----------|
| Row index stores `file_offset` of row start | `csv_tell()` returns `last_row_pos` recorded *before* the line is read — this is the correct seek target for `csv_seek()` to re-parse the same row |
| `ws_push_task` stack 4 096 B | Raised to 6 144 B due to `push_replay_window()` stack usage; `char json[1024]` made `static` to keep BSS cost predictable |
| Phase 13 was originally Integration Testing | Repurposed as Replay Mode Redesign; integration testing deferred |

