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

/**
 * @brief UART2 RX pin — wired to the RS485 Receive Output (RO) of the
 *        Atomic RS485 Base.
 */
#define RS485_RX               22

/**
 * @brief UART2 TX pin — wired to the RS485 Driver Input (DI) of the
 *        Atomic RS485 Base.
 */
#define RS485_TX               19

/**
 * @brief UART baud rate for both emulated sensors (FG6485A and S200).
 *
 * 9600 baud is the factory default for both devices and must match the baud
 * rate configured on the physical Modbus master.
 */
#define RS485_BAUD_RATE        9600

/**
 * @brief Silence duration in milliseconds after which a byte stream is
 *        treated as a complete Modbus RTU frame.
 *
 * The Modbus RTU specification requires 3.5 character times of silence, which
 * equals approximately 4 ms at 9600 baud.  This constant is set to 20 ms so
 * that it comfortably exceeds one FreeRTOS tick period (10 ms at the
 * ESP-IDF default of 100 Hz): a vTaskDelay(1) yields for a full tick, so the
 * silence window must exceed one tick to prevent premature frame termination.
 * 20 ms is still well below any realistic Modbus inter-request gap (≥ 100 ms).
 */
#define RS485_FRAME_TIMEOUT_MS  20

/**
 * @brief Initialise UART2 at RS485_BAUD_RATE baud, 8 data bits, no parity,
 *        1 stop bit (8N1).
 *
 * Must be called once from setup() before rs485_read_frame() or rs485_write().
 */
void    rs485_init(void);

/**
 * @brief Transmit @p len bytes on the RS485 bus.
 *
 * Blocks until Serial2.flush() confirms that the last byte has been shifted
 * out of the UART FIFO.  No echo drain is needed — the Atomic RS485 Base
 * uses hardware auto-direction control that disables its own Receive Output
 * (RO) while TX is asserted, so transmitted bytes never appear on RX.
 *
 * @param data  Pointer to the byte array to transmit.
 * @param len   Number of bytes to transmit.
 */
void    rs485_write(const uint8_t *data, uint8_t len);

/**
 * @brief Collect a complete Modbus RTU frame from the RS485 bus.
 *
 * Waits up to 100 ms for the first byte to arrive (master turnaround time),
 * then accumulates bytes until RS485_FRAME_TIMEOUT_MS of inter-character
 * silence is detected, signalling end-of-frame.
 *
 * @param buf     Destination buffer for the received bytes.
 * @param maxLen  Size of @p buf in bytes.
 * @return        Number of bytes written to @p buf; 0 if no frame arrived.
 */
uint8_t rs485_read_frame(uint8_t *buf, uint8_t maxLen);

/**
 * @brief Discard all bytes currently pending in the UART2 RX FIFO.
 *
 * Useful after a bus idle period or before starting a new receive cycle to
 * ensure stale bytes from a previous partial frame are not misinterpreted.
 */
void    rs485_flush_rx(void);
