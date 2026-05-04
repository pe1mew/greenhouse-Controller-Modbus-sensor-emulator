/**
 * @file rs485.cpp
 * @brief UART2 / RS485 HAL — Phase 1 implementation.
 */

#include "rs485.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void rs485_init(void)
{
    Serial2.begin(RS485_BAUD_RATE, SERIAL_8N1, RS485_RX, RS485_TX);
}

// ---------------------------------------------------------------------------
// Write + echo drain
// ---------------------------------------------------------------------------

void rs485_write(const uint8_t *data, uint8_t len)
{
    Serial2.write(data, len);
    Serial2.flush();   // Block until last bit is shifted out.
    // No echo drain: the Atomic RS485 Base disables RO during TX, so the
    // device cannot receive its own transmitted bytes.
}

// ---------------------------------------------------------------------------
// Frame receive
// ---------------------------------------------------------------------------

uint8_t rs485_read_frame(uint8_t *buf, uint8_t maxLen)
{
    uint8_t  len      = 0;
    uint32_t lastByte = millis();

    // Wait up to 200 ms for the first byte (covers master turnaround time).
    // vTaskDelay(1) passes 1 tick (not 1 ms) — on ESP-IDF the default tick rate
    // is 100 Hz (10 ms/tick), so pdMS_TO_TICKS(1) truncates to 0 ticks and does
    // not yield.  Passing 1 tick directly guarantees a real scheduler yield.
    while (!Serial2.available() && (millis() - lastByte) < 200u) {
        vTaskDelay(1);
    }

    if (!Serial2.available()) {
        return 0;   // Nothing arrived within the initial wait window.
    }

    // Accumulate bytes; return as soon as inter-character silence is detected.
    // Yield with vTaskDelay(1) (1 tick) when the FIFO is empty — never yield
    // when bytes are available so we don't miss back-to-back characters.
    lastByte = millis();
    while ((millis() - lastByte) < RS485_FRAME_TIMEOUT_MS) {
        if (Serial2.available()) {
            uint8_t b = (uint8_t)Serial2.read();
            if (len < maxLen) {
                buf[len++] = b;
            }
            lastByte = millis();
        } else {
            vTaskDelay(1);  // 1 tick — yield to IDLE; resets WDT.
        }
    }

    return len;
}

// ---------------------------------------------------------------------------
// RX flush
// ---------------------------------------------------------------------------

void rs485_flush_rx(void)
{
    while (Serial2.available()) {
        Serial2.read();
    }
}
