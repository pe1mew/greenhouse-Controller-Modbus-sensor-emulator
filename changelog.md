# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

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
