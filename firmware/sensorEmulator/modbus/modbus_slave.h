/**
 * @file modbus_slave.h
 * @brief Modbus RTU slave — frame receive, CRC validation, FC dispatch — Phase 2.
 *
 * Architecture
 * ------------
 * This module owns the RS485 bus interaction on the slave side.  It is driven
 * by a single FreeRTOS task (modbus_slave_task) that runs at the highest
 * scheduler priority.
 *
 * Two slave addresses are served simultaneously (FG6485A and S200).  Each
 * address / function-code pair may have a registered handler (added in
 * Phases 3 and 4).  In Phase 2 no handlers are registered, so every
 * addressed request returns Modbus exception 0x01 (Illegal Function).
 *
 * LED behaviour:
 *   Green blink — frame received with valid CRC (response sent or exception sent).
 *   Red   blink — frame received with invalid CRC (silently discarded).
 *
 * Slave address 0x00 (broadcast) is always silently ignored.
 * Frames addressed to neither of the two configured addresses are also silently
 * ignored (no response; the master times out).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Maximum number of (slave address, function code) handler pairs that
 *        can be registered with modbus_register_handler().
 *
 * The dispatch table is a flat array; a linear search is performed on every
 * received frame.  16 entries is sufficient for two sensors each supporting
 * up to three distinct function codes.
 */
#define MODBUS_MAX_HANDLERS  16

/**
 * @brief Prototype for a Modbus function-code handler.
 *
 * Invoked by modbus_slave_task when a valid request is received for a
 * registered (slave address, function code) pair.
 *
 * @param req      Full received frame: [addr, fc, data…, crcL, crcH].
 * @param req_len  Total length of @p req in bytes (includes the 2 CRC bytes).
 * @param resp     Caller-supplied response buffer of at least 256 bytes.
 * @param resp_len [out] Number of bytes to transmit from @p resp.
 *                       Set to 0 to suppress the response (e.g. broadcast
 *                       writes where no reply is permitted by the spec).
 */
typedef void (*modbus_fc_handler_t)(const uint8_t *req, uint8_t req_len,
                                    uint8_t *resp, uint8_t *resp_len);

/**
 * @brief Set the two slave addresses served by this device.
 *
 * Frames addressed to neither @p addr_a nor @p addr_b are silently ignored
 * (no response is sent; the master will time out).
 *
 * Defaults: @p addr_a = 1 (FG6485A), @p addr_b = 44 (S200).
 * Safe to call before xTaskCreate(modbus_slave_task, …), or at runtime —
 * uint8_t stores are atomic on the ESP32 Xtensa core.
 *
 * @param addr_a  Slave address for sensor A (FG6485A).
 * @param addr_b  Slave address for sensor B (S200).
 */
void modbus_slave_set_addrs(uint8_t addr_a, uint8_t addr_b);

/**
 * @brief Register (or replace) the handler for a specific
 *        (slave address, function code) pair.
 *
 * If an entry with the same @p slave_addr and @p fc already exists it is
 * overwritten in place.  Otherwise a new entry is appended.  The table
 * capacity is MODBUS_MAX_HANDLERS.
 *
 * @param slave_addr  Modbus slave address (1–247).
 * @param fc          Modbus function code (e.g. 0x03, 0x04, 0x10).
 * @param handler     Handler function pointer; must not be NULL.
 * @return            @c true on success; @c false if the table is full.
 */
bool modbus_register_handler(uint8_t slave_addr, uint8_t fc,
                              modbus_fc_handler_t handler);

/**
 * @brief Construct a Modbus exception response in @p resp.
 *
 * Writes the 5-byte sequence [addr, fc|0x80, exception_code, crcL, crcH] and
 * appends the correct CRC-16/IBM checksum.  @p resp must point to a buffer
 * of at least 5 bytes.  On return, @p *resp_len is set to 5.
 *
 * Common exception codes:
 *   - 0x01  Illegal Function — FC is not supported by this slave.
 *   - 0x02  Illegal Data Address — register range not implemented.
 *   - 0x03  Illegal Data Value — write value is out of range.
 *
 * @param addr            Slave address echoed from the request.
 * @param fc              Function code echoed from the request.
 * @param exception_code  Modbus exception code (0x01–0x0B).
 * @param resp            Output buffer (minimum 5 bytes).
 * @param resp_len        [out] Set to 5 on return.
 */
void modbus_build_exception(uint8_t addr, uint8_t fc, uint8_t exception_code,
                             uint8_t *resp, uint8_t *resp_len);

/**
 * @brief FreeRTOS task that drives the Modbus RTU slave engine.
 *
 * Continuously calls rs485_read_frame(), validates the CRC, looks up the
 * (slave address, function code) pair in the handler table, and calls the
 * registered handler.  If no handler is found, sends exception 0x01 (Illegal
 * Function).  If the address does not match either configured slave, the
 * frame is silently discarded.  Broadcast address 0x00 is always ignored.
 *
 * Create with at least 4096 bytes of stack at priority
 * @c configMAX_PRIORITIES - 1 so that RS485 timing is not disturbed by lower
 * priority tasks.
 *
 * @param arg  Unused; pass @c NULL.
 *
 * @par Example
 * @code
 *   xTaskCreate(modbus_slave_task, "modbus_slave", 4096, NULL,
 *               configMAX_PRIORITIES - 1, NULL);
 * @endcode
 */
void modbus_slave_task(void *arg);
