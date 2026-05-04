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

/**
 * @brief EventGroup bit set when the STA link is up and an IP address has
 *        been assigned.  Cleared when the STA disconnects.
 *
 * Tasks that require network access (ntp_task, live_fetch_task) should block
 * on this bit before attempting any network operation.
 */
#define WIFI_EVT_STA_CONNECTED    ((EventBits_t)(1u << 0))

/**
 * @brief EventGroup bit set when the STA link is lost or was never established.
 *        Cleared when STA successfully connects.
 *
 * Can be used by tasks that need to tear down network resources gracefully
 * when connectivity is lost.
 */
#define WIFI_EVT_STA_DISCONNECTED ((EventBits_t)(1u << 1))

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

/**
 * @brief WiFi manager operating state.
 *
 * Reflects the current network mode of the device.  Updated atomically by
 * @c wifi_manager_task; readable via wifi_manager_get_state().
 */
typedef enum {
    /** AP is active; no STA credentials configured or STA never connected. */
    WIFI_STATE_AP         = 0,
    /** AP is active and a STA association attempt is in progress. */
    WIFI_STATE_CONNECTING = 1,
    /** STA is connected and has a valid IP; AP is disabled; mDNS is active. */
    WIFI_STATE_STA        = 2,
} wifi_manager_state_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Initialise the WiFi manager.
 *
 * Creates the public EventGroup, the connect-request queue (capacity 1),
 * and launches @c wifi_manager_task at FreeRTOS priority 2 with an 8 KiB
 * stack.  The task immediately starts the AP and — if NVS credentials are
 * present — begins a STA connection attempt.
 *
 * Must be called once from @c setup() after nvs_cfg_init().
 */
void wifi_manager_init(void);

/**
 * @brief Request a STA connection to the given network.
 *
 * Persists @p ssid and @p pass to NVS so they are retried on the next boot.
 * Posts a connect request to the internal queue; @c wifi_manager_task picks
 * it up within 500 ms.  Safe to call from any task context.
 *
 * Intended for use by the web interface POST handler (Phase 7).
 *
 * @param ssid  Null-terminated WiFi network name (max NVS_STR_MAX_SSID − 1 chars).
 * @param pass  Null-terminated WiFi password (max NVS_STR_MAX_PASS − 1 chars).
 */
void wifi_manager_connect(const char *ssid, const char *pass);

/**
 * @brief Return the FreeRTOS EventGroup handle.
 *
 * Callers may use xEventGroupWaitBits() with @c WIFI_EVT_STA_CONNECTED or
 * @c WIFI_EVT_STA_DISCONNECTED to synchronise network-dependent tasks.
 *
 * @return  EventGroupHandle_t created by wifi_manager_init().
 */
EventGroupHandle_t wifi_manager_get_event_group(void);

/**
 * @brief Return the current WiFi manager state.
 *
 * The returned value is a snapshot; it may change immediately after the call.
 *
 * @return  One of @c WIFI_STATE_AP, @c WIFI_STATE_CONNECTING, or @c WIFI_STATE_STA.
 */
wifi_manager_state_t wifi_manager_get_state(void);

/**
 * @brief Copy the STA IP address string into @p buf.
 *
 * If the device is not currently in STA mode, @p buf is set to an empty string.
 * @p buf is always null-terminated.
 *
 * @param buf  Destination buffer for the dotted-decimal IP string.
 * @param len  Size of @p buf in bytes; must be at least 16.
 */
void wifi_manager_get_sta_ip(char *buf, size_t len);

/**
 * @brief Return a pointer to the AP SSID string.
 *
 * The returned pointer is valid for the lifetime of the firmware.  The string
 * has the form @c "SensorEmulator-XXYY" where @c XX and @c YY are the
 * hexadecimal representations of MAC bytes 4 and 5 of the AP interface.
 *
 * @return  Null-terminated AP SSID string (e.g. @c "SensorEmulator-B78D").
 */
const char *wifi_manager_get_ap_ssid(void);
