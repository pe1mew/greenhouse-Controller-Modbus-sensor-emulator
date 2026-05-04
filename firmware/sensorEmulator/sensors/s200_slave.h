/**
 * @file s200_slave.h
 * @brief SenseCAP S200 wind sensor Modbus slave handlers — Phase 4.
 *
 * Registers two function-code handlers with the Modbus slave engine:
 *   - **FC04** (Read Input Registers) — wind direction, wind speed, heating temperature.
 *   - **FC03** (Read Holding Registers) — slave address and baud rate configuration.
 *
 * Register map:
 * | Address      | FC    | Description                                            |
 * |:-------------|:------|:-------------------------------------------------------|
 * | 0x0008–0x0009 | FC04 | Wind direction minimum (int32 ×1000, deg)             |
 * | 0x000A–0x000B | FC04 | Wind direction maximum (int32 ×1000, deg)             |
 * | 0x000C–0x000D | FC04 | Wind direction average (int32 ×1000, deg)             |
 * | 0x000E–0x000F | FC04 | Wind speed minimum (int32 ×1000, m/s)                 |
 * | 0x0010–0x0011 | FC04 | Wind speed maximum (int32 ×1000, m/s)                 |
 * | 0x0012–0x0013 | FC04 | Wind speed average (int32 ×1000, m/s)                 |
 * | 0x001C–0x001D | FC04 | Heating temperature high (int32 ×1000, °C)            |
 * | 0x001E–0x001F | FC04 | Heating temperature low  (int32 ×1000, °C)            |
 * | 0x1000       | FC03  | Slave address (uint16)                                |
 * | 0x1001       | FC03  | Baud rate code (uint16; 1 = 9600 baud)                |
 *
 * int32 wire encoding: each 32-bit value is transmitted as two consecutive
 * big-endian 16-bit registers, high word first.  Example: 180000 (180.000°)
 * is sent as @c 0x0002 @c 0xBF20.
 *
 * All values are read from @c g_sensor_state under its mutex.
 */

#pragma once

#include <stdint.h>

/**
 * @brief Register FC04 and FC03 handlers for the S200 sensor.
 *
 * Adds two entries to the Modbus slave handler table: one for FC04 and one
 * for FC03, both keyed to @p addr.  Must be called after nvs_cfg_init() and
 * before xTaskCreate(modbus_slave_task, …).
 *
 * @param addr  Modbus slave address for the S200 (typically 44).
 */
void s200_slave_register(uint8_t addr);
