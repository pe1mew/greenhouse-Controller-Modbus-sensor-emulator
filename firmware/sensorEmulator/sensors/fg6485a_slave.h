/**
 * @file fg6485a_slave.h
 * @brief FG6485A Modbus slave FC handlers — Phase 3.
 *
 * Registers two handlers with the modbus_slave module:
 *   FC03 — Read Holding Registers
 *   FC16 — Write Multiple Registers
 *
 * Call fg6485a_slave_register() once after modbus_register_handler() is
 * available (i.e. before modbus_slave_task starts).
 *
 * Register map served:
 *   0x0000        humidity raw × 10 (uint16)
 *   0x0001        temperature raw × 10 (int16)
 *   0x0008–0x000B device info (read-only)
 *   0x000C–0x0013 alarm config (FC03 read / FC16 write)
 *   0x001D–0x001E correction offsets (FC16 write-only;
 *                  reading these returns exception 0x02)
 *
 * Any FC03 read that spans registers outside the defined ranges returns
 * exception 0x02 (Illegal Data Address).
 * FC16 writes outside 0x000C–0x001E return exception 0x02.
 */

#pragma once

#include <stdint.h>

/**
 * Register the FG6485A FC03 and FC16 handlers for slave address @p addr.
 * Must be called before modbus_slave_task is created.
 */
void fg6485a_slave_register(uint8_t addr);
