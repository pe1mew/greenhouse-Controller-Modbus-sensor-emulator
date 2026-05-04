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
/** @brief First FC03-readable measurement register (humidity). */
static constexpr uint16_t REG_MEAS_START    = 0x0000u;
/** @brief Last FC03-readable measurement register (temperature, inclusive). */
static constexpr uint16_t REG_MEAS_END      = 0x0001u;
/** @brief First FC03-readable device-info register. */
static constexpr uint16_t REG_INFO_START    = 0x0008u;
/** @brief Last FC03-readable device-info register (inclusive). */
static constexpr uint16_t REG_INFO_END      = 0x000Bu;
/** @brief First FC03-readable alarm-config register. */
static constexpr uint16_t REG_ALARM_START   = 0x000Cu;
/** @brief Last FC03-readable alarm-config register (inclusive). */
static constexpr uint16_t REG_ALARM_END     = 0x0013u;

// FC16-writable ranges
/** @brief First FC16-writable register. */
static constexpr uint16_t REG_WRITE_START   = 0x000Cu;
/** @brief Last FC16-writable register (inclusive). */
static constexpr uint16_t REG_WRITE_END     = 0x001Eu;

// ---------------------------------------------------------------------------
// Build a standard FC03/FC16 response header + CRC
// ---------------------------------------------------------------------------

/**
 * @brief Serialise a register-read response frame into @p resp.
 *
 * Produces a complete Modbus RTU frame:
 * @code
 *   [addr][fc][byte_count][word0_hi][word0_lo]...[crc_lo][crc_hi]
 * @endcode
 * The CRC covers all bytes from @p resp[0] to the last data byte.
 *
 * @param addr      Slave address echoed in the response.
 * @param fc        Function code echoed in the response (0x03).
 * @param words     Array of register values in big-endian word order.
 * @param qty       Number of 16-bit words in @p words.
 * @param resp      Output buffer (must be at least 3 + qty*2 + 2 bytes).
 * @param resp_len  Set to the total number of bytes written to @p resp.
 */
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
// Read a single register value from g_sensor_state
// ---------------------------------------------------------------------------

/**
 * @brief Map a single FC03 register address to its value in g_sensor_state.
 *
 * Must be called with g_sensor_state.mutex already held.  Covers the
 * measurement (0x0000–0x0001), device-info (0x0008–0x000B), and alarm-config
 * (0x000C–0x0013) readable ranges defined in the FG6485A register map.
 *
 * @param reg  Modbus register address.
 * @param out  Set to the register value on success; unchanged on failure.
 * @return @c true if the register is FC03-readable, @c false otherwise.
 */
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
// Write a single register value to g_sensor_state
// ---------------------------------------------------------------------------

/**
 * @brief Write a single FC16 register value into g_sensor_state.
 *
 * Must be called with g_sensor_state.mutex already held.  Only the
 * alarm-config (0x000C–0x0013) and correction-offset (0x001D–0x001E)
 * writable registers are accepted; all others are rejected.
 *
 * @param reg    Modbus register address.
 * @param value  16-bit value to write.
 * @return @c true if the register is FC16-writable, @c false otherwise.
 */
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

/**
 * @brief Modbus FC03 (Read Holding Registers) handler for the FG6485A slave.
 *
 * Validates quantity (1–125) and that every requested register address is
 * FC03-readable, then snapshots the values under g_sensor_state.mutex and
 * builds the response via build_read_response().  Returns exception 0x02
 * (Illegal Data Address) or 0x03 (Illegal Data Value) on bad inputs.
 *
 * @param req      Full received Modbus RTU frame (address through CRC).
 * @param req_len  Length of @p req in bytes (unused internally).
 * @param resp     Output buffer for the response frame.
 * @param resp_len Set to the number of bytes written to @p resp.
 */
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

/**
 * @brief Modbus FC16 (Write Multiple Registers) handler for the FG6485A slave.
 *
 * Validates quantity (1–123) and that every target register is FC16-writable
 * before touching the mutex.  Applies all writes atomically under
 * g_sensor_state.mutex and returns the standard FC16 echo response (8 bytes).
 * Returns exception 0x02 (Illegal Data Address) or 0x03 (Illegal Data Value)
 * on bad inputs.
 *
 * @param req      Full received Modbus RTU frame.
 * @param req_len  Length of @p req in bytes (unused internally).
 * @param resp     Output buffer for the response frame.
 * @param resp_len Set to the number of bytes written to @p resp.
 */
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
