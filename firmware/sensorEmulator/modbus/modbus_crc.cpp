/**
 * @file modbus_crc.cpp
 * @brief Modbus CRC-16/IBM implementation — Phase 2.
 */

#include "modbus_crc.h"

uint16_t modbus_crc16(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x0001u) ? (crc >> 1) ^ 0xA001u : crc >> 1;
        }
    }
    return crc;
}
