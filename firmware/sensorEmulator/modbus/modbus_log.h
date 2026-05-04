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
 * - All formatting (timestamp, hex string, summary) is done at post time
 *   inside modbus_log_post() so the push task only copies pre-built strings
 *   into JSON — no large stack buffers required there.
 * - Queue depth 8 at ~196 bytes per entry ≈ 1.6 KB RAM.
 *   At 9600 baud the Modbus master polls at most 3–4 frame pairs per second;
 *   with a 1 s push interval the queue is never more than ~8 entries deep.
 * - modbus_log_receive() is called from ws_push_task (priority 1) and
 *   may block on a non-zero wait tick.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
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
 * All fields are pre-formatted strings, built inside modbus_log_post() at
 * the moment the frame is captured.  The push task copies them directly into
 * JSON without any additional formatting work or large intermediate buffers.
 *
 * Hex string capacity: 96 bytes covers frames up to 32 bytes
 * (32 × 3 − 1 = 95 chars + NUL).  Real frames at 9600 baud are 8–29 bytes.
 */
typedef struct {
    char ts[20];      /**< @brief "YYYY-MM-DD HH:MM:SS" or "" if clock not set. */
    char dir[3];      /**< @brief "RX" or "TX". */
    char hex[96];     /**< @brief Space-separated uppercase hex, e.g. "01 03 00 00 00 02 C4 0B". */
    char summary[64]; /**< @brief Human-readable decode, e.g. "FC03 addr=1 reg=0x0000 n=2". */
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
