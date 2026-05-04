/**
 * @file modbus_crc.h
 * @brief Modbus CRC-16/IBM (Polynomial 0xA001) — Phase 2.
 */

#pragma once

#include <stdint.h>

/**
 * Compute the Modbus CRC-16/IBM checksum.
 *
 * The algorithm is identical to the one used in the reference test client
 * (documentation/code_modbusTestClient/main.cpp).
 *
 * Modbus RTU transmits the CRC low byte first, then the high byte.
 * The returned uint16_t has the low byte in bits [7:0].
 *
 * @param buf  Pointer to the data bytes.
 * @param len  Number of bytes to include in the computation.
 * @return     16-bit CRC value.
 */
uint16_t modbus_crc16(const uint8_t *buf, uint8_t len);
