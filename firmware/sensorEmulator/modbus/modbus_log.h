/**
 * @file modbus_log.h
 * @brief Modbus activity log — Phase 12.
 *
 * A FreeRTOS queue that records every received (RX) and transmitted (TX)
 * Modbus frame.  The web server drains the queue in its WebSocket push
 * loop and forwards each entry as a JSON {@c type:"log"} message so the
 * browser table stays current in near-real-time.
 *
 * Design constraints
 * ------------------
 * - modbus_log_post() is called from the highest-priority FreeRTOS task
 *   (modbus_slave_task) and MUST NOT block.  xQueueSend() is called with
 *   a zero timeout; entries are silently dropped when the queue is full.
 * - Queue depth 32 at ~330 bytes per entry ≈ 10.5 KB RAM.
 * - modbus_log_receive() is called from ws_push_task (priority 1) and
 *   may block on a non-zero wait tick.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/** @brief Direction of a logged Modbus frame. */
typedef enum {
    LOG_DIR_RX = 0, /**< @brief Frame received from Modbus master. */
    LOG_DIR_TX = 1  /**< @brief Frame transmitted to Modbus master. */
} log_dir_t;

/**
 * @brief Single entry in the Modbus activity log.
 *
 * Sized for the largest legal Modbus ADU (256 bytes) plus a short
 * human-readable summary generated from the function code and register fields.
 */
typedef struct {
    time_t    ts;         /**< @brief Unix timestamp (seconds since epoch). */
    log_dir_t dir;        /**< @brief RX or TX. */
    uint8_t   frame[256]; /**< @brief Raw frame bytes. */
    uint8_t   len;        /**< @brief Number of valid bytes in @c frame. */
    char      summary[64];/**< @brief Human-readable decode, e.g. "FC03 addr=1 reg=0x0000 n=2". */
} log_entry_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Initialise the log queue.
 *
 * Must be called once from setup() before modbus_slave_task starts.
 */
void modbus_log_init(void);

/**
 * @brief Record a Modbus frame in the log queue (non-blocking).
 *
 * Copies @p frame into a @ref log_entry_t, sets the timestamp to the
 * current wall-clock time, generates a summary string, and posts the entry
 * to the internal FreeRTOS queue with a zero timeout.  If the queue is full
 * the entry is silently discarded so the caller never blocks.
 *
 * @param dir    Direction of the frame (RX or TX).
 * @param frame  Pointer to raw frame bytes.
 * @param len    Number of bytes in @p frame (1–256).
 */
void modbus_log_post(log_dir_t dir, const uint8_t *frame, uint8_t len);

/**
 * @brief Receive the oldest pending log entry (blocking or non-blocking).
 *
 * @param out   Pointer to a @ref log_entry_t that will be populated.
 * @param wait  FreeRTOS tick count to wait if the queue is empty.
 *              Pass 0 for a non-blocking poll.
 * @return @c true if an entry was retrieved, @c false if the queue was empty.
 */
bool modbus_log_receive(log_entry_t *out, TickType_t wait);

/**
 * @brief Discard all pending entries in the log queue.
 *
 * Called by the POST /log/clear handler so the browser table and the
 * in-memory queue are both cleared atomically.
 */
void modbus_log_clear(void);

#ifdef __cplusplus
}
#endif
