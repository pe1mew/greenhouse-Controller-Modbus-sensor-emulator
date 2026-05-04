/**
 * @file s200_slave.cpp
 * @brief SenseCAP S200 Modbus slave FC04 / FC03 handlers — Phase 4.
 *
 * int32 register encoding on the bus (big-endian word order):
 *   register N   = upper 16 bits of the int32
 *   register N+1 = lower 16 bits of the int32
 *   engineering value = raw / 1000
 */

#include "s200_slave.h"
#include "sensor_state.h"
#include "../modbus/modbus_slave.h"
#include "../modbus/modbus_crc.h"

#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Build a standard FC03/FC04 read response.
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
// Map a single FC04 register address to the corresponding uint16 word.
// The int32 fields in sensor_state are split into (high-word, low-word) pairs.
// Returns false if the address is not FC04-readable.
// ---------------------------------------------------------------------------

static bool read_fc04_register(uint16_t reg, uint16_t *out)
{
    // Helper macros to extract big-endian word halves from an int32.
    // Casting to uint32_t before shifting avoids sign-extension artefacts.
#define HI(v) ((uint16_t)(((uint32_t)(v)) >> 16))
#define LO(v) ((uint16_t)((uint32_t)(v) & 0xFFFFu))

    switch (reg) {
        // Wind direction
        case 0x0008: *out = HI(g_sensor_state.s200_dir_min); return true;
        case 0x0009: *out = LO(g_sensor_state.s200_dir_min); return true;
        case 0x000A: *out = HI(g_sensor_state.s200_dir_max); return true;
        case 0x000B: *out = LO(g_sensor_state.s200_dir_max); return true;
        case 0x000C: *out = HI(g_sensor_state.s200_dir_avg); return true;
        case 0x000D: *out = LO(g_sensor_state.s200_dir_avg); return true;
        // Wind speed
        case 0x000E: *out = HI(g_sensor_state.s200_spd_min); return true;
        case 0x000F: *out = LO(g_sensor_state.s200_spd_min); return true;
        case 0x0010: *out = HI(g_sensor_state.s200_spd_max); return true;
        case 0x0011: *out = LO(g_sensor_state.s200_spd_max); return true;
        case 0x0012: *out = HI(g_sensor_state.s200_spd_avg); return true;
        case 0x0013: *out = LO(g_sensor_state.s200_spd_avg); return true;
        // Heating temperature
        case 0x001C: *out = HI(g_sensor_state.s200_heat_high); return true;
        case 0x001D: *out = LO(g_sensor_state.s200_heat_high); return true;
        case 0x001E: *out = HI(g_sensor_state.s200_heat_low);  return true;
        case 0x001F: *out = LO(g_sensor_state.s200_heat_low);  return true;
        default: return false;
    }

#undef HI
#undef LO
}

// ---------------------------------------------------------------------------
// Map a single FC03 register address (config space).
// Returns false if the address is not FC03-readable.
// ---------------------------------------------------------------------------

static bool read_fc03_register(uint16_t reg, uint16_t *out)
{
    switch (reg) {
        case 0x1000: *out = g_sensor_state.s200_slave_addr_reg; return true;
        case 0x1001: *out = g_sensor_state.s200_baud_reg;       return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// FC04 handler — input register reads (measurement data)
// ---------------------------------------------------------------------------

static void s200_fc04(const uint8_t *req, uint8_t req_len,
                       uint8_t *resp, uint8_t *resp_len)
{
    (void)req_len;
    const uint8_t  addr      = req[0];
    const uint16_t start_reg = ((uint16_t)req[2] << 8) | req[3];
    const uint16_t qty       = ((uint16_t)req[4] << 8) | req[5];

    if (qty == 0 || qty > 125) {
        modbus_build_exception(addr, 0x04, 0x03, resp, resp_len);
        return;
    }

    // Validate all requested registers before touching the mutex.
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t tmp;
        if (!read_fc04_register((uint16_t)(start_reg + i), &tmp)) {
            modbus_build_exception(addr, 0x04, 0x02, resp, resp_len);
            Serial.printf("[s200] FC04 exception 0x02 at reg 0x%04X\n", start_reg + i);
            return;
        }
    }

    uint16_t words[125];
    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < qty; i++) {
        read_fc04_register((uint16_t)(start_reg + i), &words[i]);
    }
    xSemaphoreGive(g_sensor_state.mutex);

    build_read_response(addr, 0x04, words, (uint8_t)qty, resp, resp_len);

    Serial.printf("[s200] FC04 reg=0x%04X qty=%u →", start_reg, qty);
    for (uint16_t i = 0; i < qty; i++) Serial.printf(" %04X", words[i]);
    Serial.println();
}

// ---------------------------------------------------------------------------
// FC03 handler — holding register reads (config)
// ---------------------------------------------------------------------------

static void s200_fc03(const uint8_t *req, uint8_t req_len,
                       uint8_t *resp, uint8_t *resp_len)
{
    (void)req_len;
    const uint8_t  addr      = req[0];
    const uint16_t start_reg = ((uint16_t)req[2] << 8) | req[3];
    const uint16_t qty       = ((uint16_t)req[4] << 8) | req[5];

    if (qty == 0 || qty > 125) {
        modbus_build_exception(addr, 0x03, 0x03, resp, resp_len);
        return;
    }

    for (uint16_t i = 0; i < qty; i++) {
        uint16_t tmp;
        if (!read_fc03_register((uint16_t)(start_reg + i), &tmp)) {
            modbus_build_exception(addr, 0x03, 0x02, resp, resp_len);
            Serial.printf("[s200] FC03 exception 0x02 at reg 0x%04X\n", start_reg + i);
            return;
        }
    }

    uint16_t words[125];
    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < qty; i++) {
        read_fc03_register((uint16_t)(start_reg + i), &words[i]);
    }
    xSemaphoreGive(g_sensor_state.mutex);

    build_read_response(addr, 0x03, words, (uint8_t)qty, resp, resp_len);

    Serial.printf("[s200] FC03 reg=0x%04X qty=%u →", start_reg, qty);
    for (uint16_t i = 0; i < qty; i++) Serial.printf(" %04X", words[i]);
    Serial.println();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void s200_slave_register(uint8_t addr)
{
    modbus_register_handler(addr, 0x04, s200_fc04);
    modbus_register_handler(addr, 0x03, s200_fc03);
    Serial.printf("[s200] handlers registered for addr %u\n", addr);
}
