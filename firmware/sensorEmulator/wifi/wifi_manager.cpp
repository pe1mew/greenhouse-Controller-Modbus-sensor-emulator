/**
 * @file wifi_manager.cpp
 * @brief WiFi AP/STA FSM + mDNS implementation — Phase 6.
 *
 * All WiFi and mDNS operations run inside wifi_manager_task so they are
 * confined to a single FreeRTOS task context.  The WiFi event callback
 * (which runs in the ESP-IDF event-loop task) only posts task-notification
 * bits to wifi_manager_task and never calls WiFi or mDNS APIs directly.
 */

#include "wifi_manager.h"
#include "../config/nvs_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

// ---------------------------------------------------------------------------
// Internal task-notification bits (not the public EventGroup)
// ---------------------------------------------------------------------------

/** @brief Task-notification bit set when STA obtains an IP address. */
#define NOTIFY_STA_GOT_IP  (1u << 0)
/** @brief Task-notification bit set when STA disconnects. */
#define NOTIFY_STA_DISC    (1u << 1)

// ---------------------------------------------------------------------------
// Connect-request struct — posted to s_connect_q by wifi_manager_connect()
// ---------------------------------------------------------------------------

/**
 * @brief Request record posted to the connect queue by wifi_manager_connect().
 */
typedef struct {
    char ssid[NVS_STR_MAX_SSID]; /**< @brief SSID of the network to join. */
    char pass[NVS_STR_MAX_PASS]; /**< @brief WPA2 passphrase (NUL-terminated). */
} connect_req_t;

// ---------------------------------------------------------------------------
// Module statics
// ---------------------------------------------------------------------------

/** @cond INTERNAL */
static EventGroupHandle_t          s_evt_grp     = nullptr;
static QueueHandle_t               s_connect_q   = nullptr;
static TaskHandle_t                s_task_handle = nullptr;
static volatile wifi_manager_state_t s_state     = WIFI_STATE_AP;
static char                        s_ap_ssid[32] = {};
static char                        s_sta_ip[16]  = {};
static bool                        s_mdns_up     = false;
/** @endcond */

// ---------------------------------------------------------------------------
// WiFi event callback
// ---------------------------------------------------------------------------

/**
 * @brief Translate Arduino WiFi events into FreeRTOS task-notification bits.
 *
 * Runs in the ESP-IDF event-loop task, not in wifi_manager_task, so it must
 * not call any WiFi or mDNS APIs directly.  It only calls xTaskNotify() to
 * set NOTIFY_STA_GOT_IP or NOTIFY_STA_DISC on @c s_task_handle.
 *
 * @param event  Arduino WiFi event code delivered by the ESP-IDF event loop.
 */
static void wifi_event_cb(WiFiEvent_t event)
{
    if (!s_task_handle) { return; }
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        xTaskNotify(s_task_handle, NOTIFY_STA_GOT_IP, eSetBits);
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        xTaskNotify(s_task_handle, NOTIFY_STA_DISC, eSetBits);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Manager task
// ---------------------------------------------------------------------------

/**
 * @brief WiFi/mDNS finite-state machine — runs for the lifetime of the firmware.
 *
 * All WiFi and mDNS API calls are confined to this task.  On startup it:
 *  1. Registers wifi_event_cb.
 *  2. Enables combined AP+STA mode and starts the open access point.
 *  3. Attempts a STA connection using NVS credentials (if any).
 *
 * The main loop then:
 *  - Drains the connect-request queue (populated by wifi_manager_connect()).
 *  - Waits up to 500 ms for task-notification bits set by wifi_event_cb.
 *  - Transitions between AP, CONNECTING and STA states, starts/stops mDNS,
 *    and signals the public EventGroup accordingly.
 *
 * @param arg  Unused (FreeRTOS task parameter).
 */
static void wifi_manager_task(void *arg)
{
    // Register event handler before enabling WiFi.
    WiFi.onEvent(wifi_event_cb);

    // Enable combined AP+STA mode.
    WiFi.mode(WIFI_AP_STA);

    // Build AP SSID from the last 2 bytes of the AP MAC address.
    {
        uint8_t mac[6] = {};
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid),
                 "SensorEmulator-%02X%02X", mac[4], mac[5]);
    }

    // Start the open access point (no password, design §9).
    WiFi.softAP(s_ap_ssid);
    Serial.printf("[wifi] AP started  SSID=\"%s\"  IP=%s\n",
                  s_ap_ssid, WiFi.softAPIP().toString().c_str());
    s_state = WIFI_STATE_AP;

    // Try STA connection from NVS credentials (if any).
    {
        char ssid[NVS_STR_MAX_SSID], pass[NVS_STR_MAX_PASS];
        nvs_cfg_get_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid), "");
        nvs_cfg_get_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass), "");
        if (ssid[0] != '\0') {
            Serial.printf("[wifi] STA connecting to \"%s\" (NVS)...\n", ssid);
            WiFi.begin(ssid, pass);
            s_state = WIFI_STATE_CONNECTING;
        } else {
            Serial.println("[wifi] no STA credentials in NVS — AP-only mode");
        }
    }

    // ---- Main event loop ------------------------------------------------
    for (;;) {

        // Check for an external connect request (non-blocking).
        {
            connect_req_t req;
            if (xQueueReceive(s_connect_q, &req, 0) == pdTRUE) {
                Serial.printf("[wifi] STA connecting to \"%s\" (request)...\n",
                              req.ssid);
                WiFi.disconnect(false);
                WiFi.begin(req.ssid, req.pass);
                s_state = WIFI_STATE_CONNECTING;
            }
        }

        // Wait for WiFi event notifications (500 ms timeout so the queue is
        // checked regularly even when no WiFi events arrive).
        uint32_t bits = 0;
        xTaskNotifyWait(0u, 0xFFFFFFFFu, &bits, pdMS_TO_TICKS(500));

        // ---- STA got IP -------------------------------------------------
        // Guard against duplicate GOT_IP events (ESP-IDF can fire twice when
        // mode switches occur).  If we are already in STA state, ignore.
        if ((bits & NOTIFY_STA_GOT_IP) && s_state != WIFI_STATE_STA) {
            {
                String ip = WiFi.localIP().toString();
                strncpy(s_sta_ip, ip.c_str(), sizeof(s_sta_ip) - 1);
                s_sta_ip[sizeof(s_sta_ip) - 1] = '\0';
            }
            Serial.printf("[wifi] STA connected  IP=%s\n", s_sta_ip);

            // Disable AP (design §9: AP is stopped once STA link is up).
            WiFi.softAPdisconnect(true);
            s_state = WIFI_STATE_STA;
            Serial.println("[wifi] AP disabled");

            // Register mDNS hostname "sensor-emulator" → http://sensor-emulator.local
            if (MDNS.begin("sensor-emulator")) {
                MDNS.addService("http", "tcp", 80);
                s_mdns_up = true;
                Serial.println("[wifi] mDNS started  http://sensor-emulator.local");
            } else {
                Serial.println("[wifi] mDNS MDNS.begin() failed");
            }

            // Signal other tasks.
            xEventGroupClearBits(s_evt_grp, WIFI_EVT_STA_DISCONNECTED);
            xEventGroupSetBits  (s_evt_grp, WIFI_EVT_STA_CONNECTED);
        }

        // ---- STA disconnected -------------------------------------------
        if (bits & NOTIFY_STA_DISC) {
            Serial.println("[wifi] STA disconnected");
            s_sta_ip[0] = '\0';
            xEventGroupClearBits(s_evt_grp, WIFI_EVT_STA_CONNECTED);
            xEventGroupSetBits  (s_evt_grp, WIFI_EVT_STA_DISCONNECTED);

            if (s_mdns_up) {
                MDNS.end();
                s_mdns_up = false;
                Serial.println("[wifi] mDNS stopped");
            }

            // Restart AP if it was stopped when we were in STA mode.
            if (s_state == WIFI_STATE_STA) {
                WiFi.mode(WIFI_AP_STA);
                WiFi.softAP(s_ap_ssid);
                Serial.printf("[wifi] AP restarted  SSID=\"%s\"  IP=%s\n",
                              s_ap_ssid, WiFi.softAPIP().toString().c_str());
            }
            s_state = WIFI_STATE_AP;

            // Auto-retry stored credentials so the device reconnects if the
            // network comes back.
            {
                char ssid[NVS_STR_MAX_SSID], pass[NVS_STR_MAX_PASS];
                nvs_cfg_get_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid), "");
                nvs_cfg_get_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass), "");
                if (ssid[0] != '\0') {
                    Serial.printf("[wifi] STA retrying \"%s\"...\n", ssid);
                    WiFi.begin(ssid, pass);
                    s_state = WIFI_STATE_CONNECTING;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void wifi_manager_init(void)
{
    s_evt_grp   = xEventGroupCreate();
    s_connect_q = xQueueCreate(1, sizeof(connect_req_t));
    xTaskCreate(wifi_manager_task, "wifi_mgr", 8192, nullptr, 2,
                &s_task_handle);
    Serial.println("[wifi] manager initialised");
}

void wifi_manager_connect(const char *ssid, const char *pass)
{
    // Persist credentials so they are used on next boot.
    nvs_cfg_set_str(NVS_KEY_WIFI_SSID, ssid);
    nvs_cfg_set_str(NVS_KEY_WIFI_PASS, pass);

    connect_req_t req;
    strncpy(req.ssid, ssid, sizeof(req.ssid) - 1);
    req.ssid[sizeof(req.ssid) - 1] = '\0';
    strncpy(req.pass, pass, sizeof(req.pass) - 1);
    req.pass[sizeof(req.pass) - 1] = '\0';

    if (s_connect_q) {
        // xQueueOverwrite allows posting even when the queue (size 1) is full,
        // so a new request always supersedes a pending one.
        xQueueOverwrite(s_connect_q, &req);
    }
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_evt_grp;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    return s_state;
}

void wifi_manager_get_sta_ip(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, s_sta_ip, len - 1);
        buf[len - 1] = '\0';
    }
}

const char *wifi_manager_get_ap_ssid(void)
{
    return s_ap_ssid;
}
