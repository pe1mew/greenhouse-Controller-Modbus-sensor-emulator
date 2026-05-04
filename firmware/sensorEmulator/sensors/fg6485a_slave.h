/**
 * @file fg6485a_slave.h
 * @brief FG6485A temperature/humidity sensor Modbus slave handlers — Phase 3.
 *
 * Registers two function-code handlers with the Modbus slave engine:
 *   - **FC03** (Read Holding Registers)
 *   - **FC16** (Write Multiple Registers)
 *
 * Register map:
 * | Address      | FC    | Access     | Description                               |
 * |:-------------|:------|:-----------|:------------------------------------------|
 * | 0x0000       | FC03  | Read       | Relative humidity raw (×10, uint16)        |
 * | 0x0001       | FC03  | Read       | Temperature raw (×10, int16, signed)       |
 * | 0x0008–0x000B | FC03 | Read       | Static device info (type, version, ID)    |
 * | 0x000C–0x0013 | FC03 | Read/Write | Alarm configuration                       |
 * | 0x001D–0x001E | FC16 | Write-only | Temperature and humidity correction offsets |
 *
 * Error handling:
 *   - FC03 that spans any register outside the defined ranges returns
 *     Modbus exception 0x02 (Illegal Data Address).
 *   - FC16 writes outside 0x000C–0x001E return exception 0x02.
 *
 * All values are read from (and written to) @c g_sensor_state under its mutex.
 */

#pragma once

#include <stdint.h>

/**
 * @brief Register FC03 and FC16 handlers for the FG6485A sensor.
 *
 * Adds two entries to the Modbus slave handler table: one for FC03 and one
 * for FC16, both keyed to @p addr.  Must be called after nvs_cfg_init() and
 * before xTaskCreate(modbus_slave_task, …).
 *
 * @param addr  Modbus slave address for the FG6485A (typically 1).
 */
void fg6485a_slave_register(uint8_t addr);
