/**
 * @file fg6485a_slave.cpp
 * @brief FG6485A Modbus slave FC03 / FC16 handlers — Phase 3.
 */

#include "fg6485a_slave.h"
#include "sensor_state.h"
#include "../modbus/modbus_slave.h"
#include "../modbus/modbus_crc.h"

#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Register range constants
// ---------------------------------------------------------------------------

// FC03-readable ranges
static constexpr uint16_t REG_MEAS_START    = 0x0000u;  // humidity
static constexpr uint16_t REG_MEAS_END      = 0x0001u;  // temperature  (inclusive)
static constexpr uint16_t REG_INFO_START    = 0x0008u;
static constexpr uint16_t REG_INFO_END      = 0x000Bu;
static constexpr uint16_t REG_ALARM_START   = 0x000Cu;
static constexpr uint16_t REG_ALARM_END     = 0x0013u;

// FC16-writable ranges
static constexpr uint16_t REG_WRITE_START   = 0x000Cu;
static constexpr uint16_t REG_WRITE_END     = 0x001Eu;

// ---------------------------------------------------------------------------
// Build a standard FC03/FC16 response header + CRC
// ---------------------------------------------------------------------------

static void build_read_response(uint8_t addr, uint8_t fc,
                                 const uint16_t *words, uint8_t qty,
                                 uint8_t *resp, uint8_t *resp_len)
{
    uint8_t byte_count = (uint8_t)(qty * 2u);
    resp[0] = addr;
    resp[1] = fc;
    resp[2] = byte_count;
    for (uint8_t i = 0; i < qty; i++) {
        resp[3 + i * 2]     = (uint8_t)(words[i] >> 8);
        resp[3 + i * 2 + 1] = (uint8_t)(words[i] & 0xFFu);
    }
    uint8_t frame_len = 3u + byte_count;
    uint16_t crc = modbus_crc16(resp, frame_len);
    resp[frame_len]     = (uint8_t)(crc & 0xFFu);
    resp[frame_len + 1] = (uint8_t)(crc >> 8);
    *resp_len = (uint8_t)(frame_len + 2u);
}

// ---------------------------------------------------------------------------
// Read a single register value from g_sensor_state (called under mutex)
// Returns false if address is not readable via FC03.
// ---------------------------------------------------------------------------

static bool read_register(uint16_t reg, uint16_t *out)
{
    switch (reg) {
        case 0x0000: *out = g_sensor_state.fg_humidity;        return true;
        case 0x0001: *out = (uint16_t)g_sensor_state.fg_temperature; return true;
        case 0x0008: *out = g_sensor_state.fg_device_type;     return true;
        case 0x0009: *out = g_sensor_state.fg_version;         return true;
        case 0x000A: *out = g_sensor_state.fg_device_id_high;  return true;
        case 0x000B: *out = g_sensor_state.fg_device_id_low;   return true;
        case 0x000C: *out = (uint16_t)g_sensor_state.fg_temp_alarm_hi;    return true;
        case 0x000D: *out = g_sensor_state.fg_temp_alarm_hi_en;           return true;
        case 0x000E: *out = (uint16_t)g_sensor_state.fg_temp_alarm_lo;    return true;
        case 0x000F: *out = g_sensor_state.fg_temp_alarm_lo_en;           return true;
        case 0x0010: *out = g_sensor_state.fg_hum_alarm_hi;    return true;
        case 0x0011: *out = g_sensor_state.fg_hum_alarm_hi_en; return true;
        case 0x0012: *out = g_sensor_state.fg_hum_alarm_lo;    return true;
        case 0x0013: *out = g_sensor_state.fg_hum_alarm_lo_en; return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Write a single register value to g_sensor_state (called under mutex)
// Returns false if the address is not writable via FC16.
// ---------------------------------------------------------------------------

static bool write_register(uint16_t reg, uint16_t value)
{
    switch (reg) {
        case 0x000C: g_sensor_state.fg_temp_alarm_hi    = (int16_t)value;  return true;
        case 0x000D: g_sensor_state.fg_temp_alarm_hi_en = value;           return true;
        case 0x000E: g_sensor_state.fg_temp_alarm_lo    = (int16_t)value;  return true;
        case 0x000F: g_sensor_state.fg_temp_alarm_lo_en = value;           return true;
        case 0x0010: g_sensor_state.fg_hum_alarm_hi     = value;           return true;
        case 0x0011: g_sensor_state.fg_hum_alarm_hi_en  = value;           return true;
        case 0x0012: g_sensor_state.fg_hum_alarm_lo     = value;           return true;
        case 0x0013: g_sensor_state.fg_hum_alarm_lo_en  = value;           return true;
        case 0x001D: g_sensor_state.fg_temp_correction  = (int16_t)value;  return true;
        case 0x001E: g_sensor_state.fg_hum_correction   = (int16_t)value;  return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// FC03 handler
// ---------------------------------------------------------------------------

static void fg6485a_fc03(const uint8_t *req, uint8_t req_len,
                          uint8_t *resp, uint8_t *resp_len)
{
    (void)req_len;
    const uint8_t  addr      = req[0];
    const uint16_t start_reg = ((uint16_t)req[2] << 8) | req[3];
    const uint16_t qty       = ((uint16_t)req[4] << 8) | req[5];

    // Validate quantity
    if (qty == 0 || qty > 125) {
        modbus_build_exception(addr, 0x03, 0x03, resp, resp_len);  // Illegal Data Value
        return;
    }

    // Check every requested register is FC03-readable
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t tmp;
        if (!read_register((uint16_t)(start_reg + i), &tmp)) {
            modbus_build_exception(addr, 0x03, 0x02, resp, resp_len);  // Illegal Data Address
            Serial.printf("[fg6485a] FC03 exception 0x02 at reg 0x%04X\n", start_reg + i);
            return;
        }
    }

    // Collect values under mutex
    uint16_t words[125];
    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < qty; i++) {
        read_register((uint16_t)(start_reg + i), &words[i]);
    }
    xSemaphoreGive(g_sensor_state.mutex);

    build_read_response(addr, 0x03, words, (uint8_t)qty, resp, resp_len);

    Serial.printf("[fg6485a] FC03 reg=0x%04X qty=%u → ", start_reg, qty);
    for (uint16_t i = 0; i < qty; i++) Serial.printf("%04X ", words[i]);
    Serial.println();
}

// ---------------------------------------------------------------------------
// FC16 handler
// ---------------------------------------------------------------------------

static void fg6485a_fc16(const uint8_t *req, uint8_t req_len,
                          uint8_t *resp, uint8_t *resp_len)
{
    (void)req_len;
    const uint8_t  addr      = req[0];
    const uint16_t start_reg = ((uint16_t)req[2] << 8) | req[3];
    const uint16_t qty       = ((uint16_t)req[4] << 8) | req[5];
    // req[6] = byte count

    if (qty == 0 || qty > 123) {
        modbus_build_exception(addr, 0x10, 0x03, resp, resp_len);
        return;
    }

    // Check every target register is writable
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t dummy = 0;
        if (!write_register((uint16_t)(start_reg + i), dummy)) {
            modbus_build_exception(addr, 0x10, 0x02, resp, resp_len);
            Serial.printf("[fg6485a] FC16 exception 0x02 at reg 0x%04X\n", start_reg + i);
            return;
        }
    }

    // Apply writes under mutex
    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t value = ((uint16_t)req[7 + i * 2] << 8) | req[8 + i * 2];
        write_register((uint16_t)(start_reg + i), value);
    }
    xSemaphoreGive(g_sensor_state.mutex);

    // FC16 success response: echo addr + FC + start_reg + qty + CRC (8 bytes)
    resp[0] = addr;
    resp[1] = 0x10;
    resp[2] = (uint8_t)(start_reg >> 8);
    resp[3] = (uint8_t)(start_reg & 0xFFu);
    resp[4] = (uint8_t)(qty >> 8);
    resp[5] = (uint8_t)(qty & 0xFFu);
    uint16_t crc = modbus_crc16(resp, 6);
    resp[6] = (uint8_t)(crc & 0xFFu);
    resp[7] = (uint8_t)(crc >> 8);
    *resp_len = 8;

    Serial.printf("[fg6485a] FC16 reg=0x%04X qty=%u written\n", start_reg, qty);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void fg6485a_slave_register(uint8_t addr)
{
    modbus_register_handler(addr, 0x03, fg6485a_fc03);
    modbus_register_handler(addr, 0x10, fg6485a_fc16);
    Serial.printf("[fg6485a] handlers registered for addr %u\n", addr);
}
