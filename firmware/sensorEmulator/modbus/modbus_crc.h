/**
 * @file modbus_crc.h
 * @brief Modbus CRC-16/IBM checksum \u2014 Phase 2.
 *
 * Implements the standard Modbus CRC-16/IBM algorithm:
 *   - Initial value: 0xFFFF
 *   - Polynomial:    0xA001 (bit-reversed representation of 0x8005)
 *   - Input/output reflected: yes
 *
 * The algorithm is identical to the reference implementation in
 * @c documentation/code_modbusTestClient/main.cpp.
 */

#pragma once

#include <stdint.h>

/**
 * @brief Compute the Modbus CRC-16/IBM checksum over @p len bytes.
 *
 * Modbus RTU frames append the CRC as two bytes: low byte first, then high
 * byte.  The returned @c uint16_t has the low byte in bits [7:0] so that
 * callers can write it directly with:
 * @code
 *   uint16_t crc = modbus_crc16(buf, len);
 *   buf[len]     = (uint8_t)(crc & 0xFF);   // low byte first
 *   buf[len + 1] = (uint8_t)(crc >> 8);     // high byte second
 * @endcode
 *
 * @param buf  Pointer to the start of the data bytes.
 * @param len  Number of bytes to include in the computation.
 * @return     16-bit CRC value; low byte in bits [7:0].
 */
uint16_t modbus_crc16(const uint8_t *buf, uint8_t len);
