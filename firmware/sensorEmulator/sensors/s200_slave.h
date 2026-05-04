/**
 * @file s200_slave.h
 * @brief SenseCAP S200 Modbus slave handlers — Phase 4.
 *
 * Provides:
 *   FC04 — input-register reads (wind direction/speed/heating temp)
 *   FC03 — holding-register reads (slave address, baud rate config)
 *
 * Register map served:
 *   FC04  0x0008–0x0009   Wind direction minimum   (int32 × 1000, big-endian words)
 *   FC04  0x000A–0x000B   Wind direction maximum
 *   FC04  0x000C–0x000D   Wind direction average
 *   FC04  0x000E–0x000F   Wind speed minimum       (int32 × 1000, m/s)
 *   FC04  0x0010–0x0011   Wind speed maximum
 *   FC04  0x0012–0x0013   Wind speed average
 *   FC04  0x001C–0x001D   Heating temperature high (int32 × 1000, °C)
 *   FC04  0x001E–0x001F   Heating temperature low  (int32 × 1000, °C)
 *   FC03  0x1000           Slave address (uint16)
 *   FC03  0x1001           Baud rate code (uint16, 1 = 9600)
 *
 * All values are read from g_sensor_state under its mutex.
 */

#pragma once

#include <stdint.h>

/**
 * Register FC04 and FC03 handlers for the S200 at the given slave address.
 * Call once from setup() before starting the modbus_slave_task.
 */
void s200_slave_register(uint8_t addr);
