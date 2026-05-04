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

/** Maximum number of (addr, FC) handler registrations. */
#define MODBUS_MAX_HANDLERS  16

/**
 * FC handler function type.
 *
 * Called by modbus_slave_task when a valid request is received for a
 * registered (slave_addr, fc) pair.
 *
 * @param req      Full received frame: [addr, fc, data…, crcL, crcH].
 * @param req_len  Total frame length in bytes (includes 2 CRC bytes).
 * @param resp     Caller-supplied buffer of at least 256 bytes for the response.
 * @param resp_len [out] Set to the number of bytes to transmit.
 *                       Set to 0 to send no response (e.g. broadcast writes).
 */
typedef void (*modbus_fc_handler_t)(const uint8_t *req, uint8_t req_len,
                                    uint8_t *resp, uint8_t *resp_len);

/**
 * Set the two slave addresses served by this device.
 *
 * Defaults: addr_a = 1 (FG6485A), addr_b = 44 (S200).
 * Safe to call before xTaskCreate(modbus_slave_task, …), or at runtime —
 * uint8_t assignments are atomic on the ESP32 Xtensa core.
 */
void modbus_slave_set_addrs(uint8_t addr_a, uint8_t addr_b);

/**
 * Register (or replace) the handler for a specific (slave_addr, fc) pair.
 *
 * @return true on success; false if MODBUS_MAX_HANDLERS entries are already used.
 */
bool modbus_register_handler(uint8_t slave_addr, uint8_t fc,
                              modbus_fc_handler_t handler);

/**
 * Build a Modbus exception response into resp[].
 *
 * Writes [addr, fc|0x80, exception_code, crcL, crcH] (5 bytes total) and
 * appends the correct CRC.  resp must point to a buffer of at least 5 bytes.
 * On return, *resp_len is set to 5.
 */
void modbus_build_exception(uint8_t addr, uint8_t fc, uint8_t exception_code,
                             uint8_t *resp, uint8_t *resp_len);

/**
 * FreeRTOS task entry point.
 *
 * Create with at least 4096 bytes of stack at priority configMAX_PRIORITIES - 1.
 * The task argument is unused (pass NULL).
 *
 * Example:
 *   xTaskCreate(modbus_slave_task, "modbus_slave", 4096, NULL,
 *               configMAX_PRIORITIES - 1, NULL);
 */
void modbus_slave_task(void *arg);
