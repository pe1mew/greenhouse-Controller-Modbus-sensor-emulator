/**
 * @file rs485.h
 * @brief UART2 / RS485 HAL — Phase 1.
 *
 * Thin wrapper around Serial2 (UART2) for the Atomic RS485 Base.
 *
 * Hardware:
 *   G22 — UART2 RX  ← RS485 RO
 *   G19 — UART2 TX  → RS485 DI
 *   Direction control is hardware-automatic (no DE/RE pin needed).
 *
 * The Atomic RS485 Base uses hardware auto-direction control: the DE/RE pins
 * are driven by a transistor tied to the TX line.  When TX is asserted DE goes
 * HIGH (transmit enabled) and RE goes HIGH (receiver disabled), so the device
 * CANNOT receive its own transmitted bytes.  There is no self-echo to drain.
 *
 * Frame detection:
 *   rs485_read_frame() accumulates bytes and returns when the inter-character
 *   silence exceeds RS485_FRAME_TIMEOUT_MS (3.5 character times at 9600 baud
 *   ≈ 4 ms; we use 5 ms for margin).
 */

#pragma once

#include <stdint.h>

/** UART2 pin assignments (Atomic RS485 Base). */
#define RS485_RX               22
#define RS485_TX               19

/** Bus parameters — must match both emulated sensors. */
#define RS485_BAUD_RATE        9600

/**
 * Inter-character silence threshold that marks end-of-frame (ms).
 * Modbus RTU requires 3.5 char times = ~4 ms at 9600 baud.
 * Set to 20 ms to accommodate the ESP-IDF default FreeRTOS tick period of
 * 10 ms — a vTaskDelay(1) yields for one full tick, so the silence detection
 * window must be larger than one tick to avoid premature frame termination.
 * 20 ms is still far below any realistic inter-request gap (≥ 100 ms).
 */
#define RS485_FRAME_TIMEOUT_MS  20

/** Initialise UART2 at 9600 baud 8N1. */
void    rs485_init(void);

/**
 * Write len bytes to the bus.
 * Blocks until Serial2.flush() confirms the last byte has been shifted out.
 * No echo drain is performed — the Atomic RS485 Base disables its own
 * receiver (RO) during transmission, so no self-echo bytes appear on RX.
 */
void    rs485_write(const uint8_t *data, uint8_t len);

/**
 * Collect a complete Modbus frame from RX.
 *
 * Waits up to 100 ms for the first byte (master turnaround time), then
 * accumulates bytes until RS485_FRAME_TIMEOUT_MS of silence is detected.
 *
 * @param buf     Destination buffer.
 * @param maxLen  Size of buf.
 * @return        Number of bytes placed in buf; 0 if nothing arrived.
 */
uint8_t rs485_read_frame(uint8_t *buf, uint8_t maxLen);

/** Discard all bytes currently pending in the RX FIFO. */
void    rs485_flush_rx(void);
