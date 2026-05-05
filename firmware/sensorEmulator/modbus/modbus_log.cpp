/**
 * @file modbus_log.cpp
 * @brief Modbus activity log implementation — Phase 12.
 *
 * A FreeRTOS queue of depth LOG_QUEUE_SIZE holds log_entry_t records.
 * modbus_log_post() is always non-blocking (timeout 0) to avoid stalling
 * the high-priority Modbus slave task.
 */

#include "modbus_log.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <time.h>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/**
 * @brief Number of log entries the queue can hold before entries are dropped.
 *
 * At 9600 baud the master sends at most ~3 frame pairs per second.  With a
 * 1 s push interval we never accumulate more than ~6–8 entries between drains.
 */
static constexpr int LOG_QUEUE_SIZE = 8;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

/** @cond INTERNAL */
static QueueHandle_t s_log_queue = nullptr;
/** @endcond */

// ---------------------------------------------------------------------------
// Summary builder
// ---------------------------------------------------------------------------

/**
 * @brief Decode a raw Modbus frame into a human-readable summary string.
 *
 * Handles the most common function codes:
 *   FC01/FC02 — Read Coils / Discrete Inputs
 *   FC03/FC04 — Read Holding/Input Registers
 *   FC05      — Write Single Coil
 *   FC06      — Write Single Register
 *   FC16 (0x10) — Write Multiple Registers
 *
 * Exception responses (FC | 0x80) are decoded as "FCxx EXCEPTION code=n".
 * Unknown or short frames fall back to "FCxx addr=n".
 *
 * @param frame  Raw frame bytes.
 * @param len    Number of valid bytes in @p frame.
 * @param buf    Destination buffer.
 * @param bufsz  Size of @p buf in bytes.
 */
static void build_summary(const uint8_t *frame, uint8_t len,
                           char *buf, size_t bufsz)
{
    if (len < 2) {
        snprintf(buf, bufsz, "short frame (%u byte%s)", len, len == 1 ? "" : "s");
        return;
    }

    const uint8_t addr = frame[0];
    const uint8_t fc   = frame[1];

    // Exception response: fc bit7 set.
    if (fc & 0x80u) {
        uint8_t exc = (len >= 3) ? frame[2] : 0;
        snprintf(buf, bufsz, "FC%02X EXCEPTION addr=%u code=%u",
                 fc & 0x7Fu, addr, exc);
        return;
    }

    // Read request / response with register address + quantity (FC01–FC04).
    if ((fc == 0x01 || fc == 0x02 || fc == 0x03 || fc == 0x04) && len >= 6) {
        uint16_t reg = ((uint16_t)frame[2] << 8) | frame[3];
        uint16_t qty = ((uint16_t)frame[4] << 8) | frame[5];
        snprintf(buf, bufsz, "FC%02X addr=%u reg=0x%04X n=%u", fc, addr, reg, qty);
        return;
    }

    // Write Single Coil / Register (FC05, FC06).
    if ((fc == 0x05 || fc == 0x06) && len >= 6) {
        uint16_t reg = ((uint16_t)frame[2] << 8) | frame[3];
        uint16_t val = ((uint16_t)frame[4] << 8) | frame[5];
        snprintf(buf, bufsz, "FC%02X addr=%u reg=0x%04X val=0x%04X", fc, addr, reg, val);
        return;
    }

    // Write Multiple Registers FC16 (0x10).
    if (fc == 0x10 && len >= 6) {
        uint16_t reg = ((uint16_t)frame[2] << 8) | frame[3];
        uint16_t qty = ((uint16_t)frame[4] << 8) | frame[5];
        snprintf(buf, bufsz, "FC10 addr=%u reg=0x%04X n=%u", addr, reg, qty);
        return;
    }

    // Fallback.
    snprintf(buf, bufsz, "FC%02X addr=%u", fc, addr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void modbus_log_init(void)
{
    s_log_queue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(log_entry_t));
    if (!s_log_queue) {
        Serial.println("[modbus_log] queue create failed");
    } else {
        Serial.printf("[modbus_log] queue ready (depth=%d, entry=%u B)\n",
                      LOG_QUEUE_SIZE, (unsigned)sizeof(log_entry_t));
    }
}

void modbus_log_post(log_dir_t dir, const uint8_t *frame, uint8_t len)
{
    if (!s_log_queue || !frame || len == 0) return;

    log_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    // Format timestamp now, in the caller's context (high-prio slave task).
    // This keeps the push task's stack free of large intermediate buffers.
    time_t now = time(nullptr);
    if (now > 1577836800LL) {   // > 2020-01-01 — NTP-synced wall clock
        struct tm ti;
        localtime_r(&now, &ti);
        strftime(entry.ts, sizeof(entry.ts), "%Y-%m-%d %H:%M:%S", &ti);
    } else {
        // Clock not yet set — fall back to device uptime in +HH:MM:SS format.
        uint32_t up_s = (uint32_t)(millis() / 1000UL);
        snprintf(entry.ts, sizeof(entry.ts), "+%02u:%02u:%02u",
                 up_s / 3600u, (up_s % 3600u) / 60u, up_s % 60u);
    }

    // Direction string.
    strncpy(entry.dir, (dir == LOG_DIR_TX) ? "TX" : "RX", sizeof(entry.dir) - 1);

    // Pre-format hex string: "AA BB CC ..."
    // Cap at the hex buffer capacity (96 bytes → up to 32 raw bytes).
    uint8_t cap = len;
    if ((size_t)(cap * 3) > sizeof(entry.hex)) {
        cap = (uint8_t)((sizeof(entry.hex) - 1) / 3);
    }
    size_t pos = 0;
    for (uint8_t i = 0; i < cap; i++) {
        int w = snprintf(entry.hex + pos, sizeof(entry.hex) - pos,
                         (i + 1 < cap) ? "%02X " : "%02X", frame[i]);
        if (w > 0) pos += (size_t)w;
    }

    // Human-readable summary.
    build_summary(frame, len, entry.summary, sizeof(entry.summary));

    // Non-blocking: silently drop if the queue is full.
    xQueueSend(s_log_queue, &entry, 0);
}

bool modbus_log_receive(log_entry_t *out, TickType_t wait)
{
    if (!s_log_queue || !out) return false;
    return xQueueReceive(s_log_queue, out, wait) == pdTRUE;
}

void modbus_log_clear(void)
{
    if (s_log_queue) {
        xQueueReset(s_log_queue);
    }
}
