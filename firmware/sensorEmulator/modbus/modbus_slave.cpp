/**
 * @file modbus_slave.cpp
 * @brief Modbus RTU slave implementation — Phase 2.
 */

#include "modbus_slave.h"
#include "modbus_crc.h"
#include "../hal/rs485.h"
#include "../hal/led.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Slave address configuration
// ---------------------------------------------------------------------------

static uint8_t s_addr_a = 1;    // FG6485A (default)
static uint8_t s_addr_b = 44;   // S200 (default, 44 = 0x2C)

void modbus_slave_set_addrs(uint8_t addr_a, uint8_t addr_b)
{
    s_addr_a = addr_a;
    s_addr_b = addr_b;
}

// ---------------------------------------------------------------------------
// Handler table
// ---------------------------------------------------------------------------

struct handler_entry_t {
    uint8_t              addr;
    uint8_t              fc;
    modbus_fc_handler_t  fn;
};

static handler_entry_t s_handlers[MODBUS_MAX_HANDLERS];
static uint8_t         s_handler_count = 0;

bool modbus_register_handler(uint8_t slave_addr, uint8_t fc,
                              modbus_fc_handler_t handler)
{
    // Replace existing entry for the same (addr, fc) pair.
    for (uint8_t i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].addr == slave_addr && s_handlers[i].fc == fc) {
            s_handlers[i].fn = handler;
            return true;
        }
    }
    // Append new entry.
    if (s_handler_count >= MODBUS_MAX_HANDLERS) {
        return false;
    }
    s_handlers[s_handler_count] = { slave_addr, fc, handler };
    s_handler_count++;
    return true;
}

static modbus_fc_handler_t find_handler(uint8_t addr, uint8_t fc)
{
    for (uint8_t i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].addr == addr && s_handlers[i].fc == fc) {
            return s_handlers[i].fn;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Exception builder
// ---------------------------------------------------------------------------

void modbus_build_exception(uint8_t addr, uint8_t fc, uint8_t exception_code,
                             uint8_t *resp, uint8_t *resp_len)
{
    resp[0] = addr;
    resp[1] = fc | 0x80u;
    resp[2] = exception_code;
    uint16_t crc = modbus_crc16(resp, 3);
    resp[3] = (uint8_t)(crc & 0xFFu);
    resp[4] = (uint8_t)(crc >> 8);
    *resp_len = 5;
}

// ---------------------------------------------------------------------------
// Serial hex-dump helper
// ---------------------------------------------------------------------------

static void log_frame(const char *prefix, const uint8_t *buf, uint8_t len)
{
    Serial.print(prefix);
    for (uint8_t i = 0; i < len; i++) {
        Serial.printf(" %02X", buf[i]);
    }
    Serial.println();
}

// ---------------------------------------------------------------------------
// Modbus slave task
// ---------------------------------------------------------------------------

void modbus_slave_task(void * /*arg*/)
{
    Serial.printf("[modbus] slave started  addr_a=%u  addr_b=%u\n",
                  s_addr_a, s_addr_b);

    static uint8_t req[256];
    static uint8_t resp[256];

    for (;;) {
        // ---- Receive frame ------------------------------------------------
        uint8_t req_len = rs485_read_frame(req, (uint8_t)(sizeof(req) - 1));

        if (req_len == 0) {
            continue;   // Timeout — nothing arrived; yield and retry.
        }

        // Minimum Modbus frame: addr(1) + fc(1) + crcL(1) + crcH(1) = 4 bytes.
        if (req_len < 4) {
            Serial.printf("[modbus] short/empty frame %u byte(s)\n", req_len);
            continue;
        }

        const uint8_t addr = req[0];
        const uint8_t fc   = req[1];

        // ---- CRC validation -----------------------------------------------
        uint16_t rx_crc   = (uint16_t)req[req_len - 2]
                          | ((uint16_t)req[req_len - 1] << 8);
        uint16_t calc_crc = modbus_crc16(req, (uint8_t)(req_len - 2));

        if (rx_crc != calc_crc) {
            log_frame("[modbus] BAD CRC →", req, req_len);
            Serial.printf("         rx=0x%04X  calc=0x%04X\n", rx_crc, calc_crc);
            led_blink(CRGB::Red);
            continue;
        }

        // ---- Broadcast: silently ignore -----------------------------------
        if (addr == 0x00) {
            continue;
        }

        // ---- Not our address: silently ignore -----------------------------
        if (addr != s_addr_a && addr != s_addr_b) {
            continue;
        }

        // ---- Valid frame for our address ----------------------------------
        log_frame("[modbus] RX →", req, req_len);

        uint8_t resp_len = 0;
        modbus_fc_handler_t handler = find_handler(addr, fc);

        if (handler != nullptr) {
            handler(req, req_len, resp, &resp_len);
        } else {
            // No handler for this FC → exception 0x01 Illegal Function.
            modbus_build_exception(addr, fc, 0x01, resp, &resp_len);
            Serial.printf("[modbus] FC 0x%02X not handled → exception 0x01\n", fc);
        }

        if (resp_len > 0) {
            log_frame("[modbus] TX →", resp, resp_len);
            rs485_write(resp, resp_len);
        }

        led_blink(CRGB::Green);
    }
}
