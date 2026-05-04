/**
 * @file wifi_manager.h
 * @brief WiFi AP/STA FSM + mDNS — Phase 6.
 *
 * Boot behaviour (design §9):
 *   1. Start open AP: SSID = "SensorEmulator-<last2MACbytes>" (uppercase hex)
 *      IP 192.168.4.1 (ESP-IDF default).
 *   2. If NVS contains wifi_ssid + wifi_pass, attempt STA connection
 *      concurrently (AP remains active during the attempt).
 *   3. On STA GOT_IP:
 *      - Disable AP (switch to STA-only mode).
 *      - Register mDNS hostname "emulator" → http://emulator.local
 *        with service _http._tcp port 80.
 *      - Set WIFI_EVT_STA_CONNECTED in the public EventGroup.
 *   4. On STA disconnect:
 *      - Stop mDNS.
 *      - Restart AP.
 *      - Set WIFI_EVT_STA_DISCONNECTED in the public EventGroup.
 *      - Retry NVS credentials automatically.
 *
 * The public EventGroup is consumed by:
 *   - ntp_task       (Phase 9): waits for WIFI_EVT_STA_CONNECTED to start SNTP.
 *   - live_fetch_task (Phase 10): resumes when STA is connected.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// ---------------------------------------------------------------------------
// Public EventGroup bit definitions
// ---------------------------------------------------------------------------

/** Set when STA is connected and has an IP.  Cleared on disconnect. */
#define WIFI_EVT_STA_CONNECTED    ((EventBits_t)(1u << 0))

/** Set when STA disconnects (or was never connected).  Cleared on connect. */
#define WIFI_EVT_STA_DISCONNECTED ((EventBits_t)(1u << 1))

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef enum {
    WIFI_STATE_AP         = 0,  /**< AP-only; no STA credentials or connecting. */
    WIFI_STATE_CONNECTING = 1,  /**< AP active; STA association in progress.    */
    WIFI_STATE_STA        = 2,  /**< STA connected; AP disabled; mDNS active.   */
} wifi_manager_state_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * Initialise the WiFi manager.
 *
 * Creates the public EventGroup, the connect-request queue, and launches
 * wifi_manager_task at priority 2 with 8 KB stack.
 * Call once from setup() after nvs_cfg_init().
 */
void wifi_manager_init(void);

/**
 * Request an STA connection.
 *
 * Saves ssid/pass to NVS (persists for next boot) and posts a connect
 * request to the manager task.  Safe to call from any task context.
 * Used by the web POST handler (Phase 7).
 */
void wifi_manager_connect(const char *ssid, const char *pass);

/** Return the shared EventGroup handle (WIFI_EVT_STA_CONNECTED / _DISCONNECTED). */
EventGroupHandle_t wifi_manager_get_event_group(void);

/** Return the current WiFi manager state. */
wifi_manager_state_t wifi_manager_get_state(void);

/**
 * Copy the STA IP address string into buf, or "" if not connected.
 * buf must be at least 16 bytes.
 */
void wifi_manager_get_sta_ip(char *buf, size_t len);

/** Return the AP SSID string (e.g. "SensorEmulator-B78C"). */
const char *wifi_manager_get_ap_ssid(void);
