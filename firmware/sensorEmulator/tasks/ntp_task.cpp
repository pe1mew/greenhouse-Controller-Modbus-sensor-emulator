/**
 * @file ntp_task.cpp
 * @brief SNTP time synchronisation + timezone management — Phase 9.
 *
 * Task lifecycle:
 *   1. Read @c tz_posix from NVS; apply via setenv / tzset (UTC0 if empty).
 *   2. Register the SNTP sync callback once (survives stop/start cycles).
 *   3. Loop:
 *      a. Wait for WIFI_EVT_STA_CONNECTED (bit stays set until disconnect).
 *      b. Read @c ntp_server from NVS into a static buffer, start SNTP.
 *      c. Wait for WIFI_EVT_STA_DISCONNECTED (bit stays set until reconnect).
 *      d. Stop SNTP, clear @c s_ntp_synced.
 *
 * @note sntp_setservername() stores a raw pointer — the server string is
 *       kept in the module-level static buffer @c s_server_buf so the
 *       pointer remains valid across SNTP stop/start cycles.
 */

#include "ntp_task.h"
#include "../wifi/wifi_manager.h"
#include "../config/nvs_config.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <time.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

/** Set to @c true by sntp_sync_cb(); cleared by stop_sntp(). */
static volatile bool s_ntp_synced   = false;

/** Guard so start_sntp() / stop_sntp() are idempotent. */
static bool          s_sntp_running = false;

/**
 * @brief NTP hostname buffer.
 *
 * Pointer passed to sntp_setservername() must remain valid for the
 * lifetime of the SNTP engine.  Static storage satisfies that requirement.
 */
static char s_server_buf[NVS_STR_MAX_NTP];

// ---------------------------------------------------------------------------
// SNTP sync callback
// ---------------------------------------------------------------------------

/**
 * @brief Called by the lwIP SNTP engine when the system clock is updated.
 *
 * Runs in the lwIP timer task — must not block or allocate.
 *
 * @param tv  New system time (already committed to the RTC by SNTP).
 */
static void sntp_sync_cb(struct timeval * /*tv*/)
{
    s_ntp_synced = true;
    Serial.println("[ntp] clock synchronised");
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * @brief Read tz_posix from NVS and apply it via setenv / tzset.
 *
 * Falls back to @c "UTC0" when the NVS key is empty or absent, and prints
 * a warning so the operator knows no timezone has been configured.
 */
static void apply_tz_from_nvs(void)
{
    char tz_buf[NVS_STR_MAX_TZ] = "";
    nvs_cfg_get_str(NVS_KEY_TZ_POSIX, tz_buf, sizeof(tz_buf), "");

    const char *tz = (tz_buf[0] != '\0') ? tz_buf : "UTC0";
    setenv("TZ", tz, 1);
    tzset();

    if (tz_buf[0] != '\0') {
        Serial.printf("[ntp] TZ applied from NVS: %s\n", tz);
    } else {
        Serial.println("[ntp] no TZ in NVS — using UTC0");
    }
}

/**
 * @brief Configure and start SNTP using the NTP server stored in NVS.
 *
 * Reads @c NVS_KEY_NTP_SERVER into the static @c s_server_buf so the
 * pointer passed to sntp_setservername() is valid until the next call to
 * stop_sntp().  No-op if SNTP is already running.
 */
static void start_sntp(void)
{
    if (s_sntp_running) return;

    nvs_cfg_get_str(NVS_KEY_NTP_SERVER, s_server_buf,
                    sizeof(s_server_buf), "pool.ntp.org");

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, s_server_buf);
    sntp_init();

    s_sntp_running = true;
    Serial.printf("[ntp] SNTP started  server=%s\n", s_server_buf);
}

/**
 * @brief Stop SNTP and clear the ntp_synced flag.
 *
 * No-op if SNTP is not currently running.
 */
static void stop_sntp(void)
{
    if (!s_sntp_running) return;

    sntp_stop();
    s_sntp_running = false;
    s_ntp_synced   = false;

    Serial.println("[ntp] SNTP stopped");
}

// ---------------------------------------------------------------------------
// Task body
// ---------------------------------------------------------------------------

/**
 * @brief NTP synchronisation task — runs for the firmware lifetime.
 *
 * Monitors the WiFi EventGroup; starts SNTP when STA connects and stops
 * it when STA disconnects.
 *
 * @param arg  Unused.
 */
static void ntp_task(void * /*arg*/)
{
    /* Apply timezone before any formatted-time output. */
    apply_tz_from_nvs();

    /* Register the sync callback once — it survives stop/start cycles. */
    sntp_set_time_sync_notification_cb(sntp_sync_cb);

    EventGroupHandle_t eg = wifi_manager_get_event_group();

    for (;;) {
        /* Block until WiFi STA is connected.
           Bit stays set until wifi_manager clears it on disconnect. */
        xEventGroupWaitBits(eg, WIFI_EVT_STA_CONNECTED,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        start_sntp();

        /* Block until WiFi STA disconnects.
           Bit stays set until wifi_manager clears it on reconnect. */
        xEventGroupWaitBits(eg, WIFI_EVT_STA_DISCONNECTED,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        stop_sntp();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ntp_task_init(void)
{
    xTaskCreate(ntp_task, "ntp_task", 3072, nullptr, 2, nullptr);
}

bool ntp_is_synced(void)
{
    return s_ntp_synced;
}
