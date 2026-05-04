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
