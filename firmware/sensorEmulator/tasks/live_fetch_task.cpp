/**
 * @file live_fetch_task.cpp
 * @brief Open-Meteo weather fetch task — Phase 10.
 *
 * Periodically queries:
 *   https://api.open-meteo.com/v1/forecast
 *     ?latitude=<lat>&longitude=<lon>
 *     &current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_direction_10m
 *     &wind_speed_unit=ms
 *
 * Parses current weather and injects clamped values into g_sensor_state for
 * every sensor whose mode is SENSOR_MODE_LIVE at the time of injection.
 *
 * On each new WiFi-STA connection, geo_ip_get_location() is called to refresh
 * the stored lat/lon from ip-api.com; this can be overridden at any time via
 * POST /config/location.
 */

#include "live_fetch_task.h"
#include "../sensors/sensor_state.h"
#include "../config/nvs_config.h"
#include "../wifi/wifi_manager.h"
#include "../net/geo_ip.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** @brief Minimum interval between Open-Meteo fetches in seconds.
 *  87 s = ~993 calls/day, safely below the Open-Meteo free-tier limit. */
static constexpr uint32_t LIVE_FETCH_INTERVAL_S = 87;

/** @brief HTTP connect + response timeout in milliseconds. */
static constexpr int FETCH_TIMEOUT_MS = 10000;

/** @brief Stack size in bytes for the live-fetch task. */
static constexpr uint32_t LIVE_FETCH_STACK = 8192;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static TaskHandle_t s_handle          = nullptr;
static uint32_t     s_last_ip         = 0;          // Track WiFi IP changes for geo-IP refresh.
static TickType_t   s_last_fetch_tick = 0;          // Tick of last successful fetch (0 = never).
static bool         s_last_fetch_ok   = false;      // Result of the most recent fetch attempt.

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** @brief Return true if either sensor is currently in LIVE mode. */
static bool any_sensor_live(void)
{
    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
    bool live = (g_sensor_state.fg_mode   == SENSOR_MODE_LIVE) ||
                (g_sensor_state.s200_mode == SENSOR_MODE_LIVE);
    xSemaphoreGive(g_sensor_state.mutex);
    return live;
}

/**
 * @brief Clamp a float to [lo, hi] and return as int32_t.
 */
static inline int32_t clamp_to_i32(float v, int32_t lo, int32_t hi)
{
    if (v < (float)lo) return lo;
    if (v > (float)hi) return hi;
    return (int32_t)v;
}

// ---------------------------------------------------------------------------
// Open-Meteo fetch
// ---------------------------------------------------------------------------

/**
 * @brief Query Open-Meteo and inject live values into g_sensor_state.
 *
 * @return true on a successful fetch and inject, false on any error.
 */
static bool fetch_and_inject(void)
{
    float lat = nvs_cfg_get_float(NVS_KEY_LIVE_LAT, 52.37f);
    float lon = nvs_cfg_get_float(NVS_KEY_LIVE_LON,  4.90f);

    // Build URL — current fields only, wind speed in m/s.
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m"
             ",wind_speed_10m,wind_direction_10m"
             "&wind_speed_unit=ms",
             lat, lon);

    WiFiClientSecure sec_client;
    sec_client.setInsecure();   // Accept any server cert — acceptable for a
                                 // non-sensitive public weather API.
    HTTPClient http;
    http.begin(sec_client, url);
    http.setTimeout(FETCH_TIMEOUT_MS);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[live_fetch] HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    // Parse JSON response.
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) {
        Serial.println("[live_fetch] JSON parse error");
        return false;
    }

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (!cJSON_IsObject(cur)) {
        Serial.println("[live_fetch] Missing 'current' object");
        cJSON_Delete(root);
        return false;
    }

    cJSON *j_temp  = cJSON_GetObjectItem(cur, "temperature_2m");
    cJSON *j_hum   = cJSON_GetObjectItem(cur, "relative_humidity_2m");
    cJSON *j_spd   = cJSON_GetObjectItem(cur, "wind_speed_10m");
    cJSON *j_dir   = cJSON_GetObjectItem(cur, "wind_direction_10m");

    if (!cJSON_IsNumber(j_temp) || !cJSON_IsNumber(j_hum) ||
        !cJSON_IsNumber(j_spd)  || !cJSON_IsNumber(j_dir))
    {
        Serial.println("[live_fetch] Missing numeric fields in 'current'");
        cJSON_Delete(root);
        return false;
    }

    float temp_c = (float)j_temp->valuedouble;   // °C
    float hum_pct = (float)j_hum->valuedouble;   // %
    float spd_ms  = (float)j_spd->valuedouble;   // m/s (requested via param)
    float dir_deg = (float)j_dir->valuedouble;   // °

    cJSON_Delete(root);

    // Convert and clamp to raw register encoding.
    // FG6485A: ×10 encoding.
    int32_t fg_temp_raw = clamp_to_i32(temp_c * 10.0f,
                                        FG6485A_TEMP_RAW_MIN,
                                        FG6485A_TEMP_RAW_MAX);
    int32_t fg_hum_raw  = clamp_to_i32(hum_pct * 10.0f,
                                        FG6485A_HUM_RAW_MIN,
                                        FG6485A_HUM_RAW_MAX);

    // S200: ×1000 encoding.
    int32_t s200_spd_raw  = clamp_to_i32(spd_ms  * 1000.0f,
                                          S200_SPD_RAW_MIN,
                                          S200_SPD_RAW_MAX);
    int32_t s200_dir_raw  = clamp_to_i32(dir_deg * 1000.0f,
                                          S200_DIR_RAW_MIN,
                                          S200_DIR_RAW_MAX);
    int32_t s200_heat_raw = clamp_to_i32(temp_c  * 1000.0f,
                                          S200_HEAT_RAW_MIN,
                                          S200_HEAT_RAW_MAX);

    // Inject into sensor_state — only for sensors currently in LIVE mode.
    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);

    if (g_sensor_state.fg_mode == SENSOR_MODE_LIVE) {
        g_sensor_state.fg_temperature = (int16_t)fg_temp_raw;
        g_sensor_state.fg_humidity    = (uint16_t)fg_hum_raw;
    }

    if (g_sensor_state.s200_mode == SENSOR_MODE_LIVE) {
        g_sensor_state.s200_spd_min =
        g_sensor_state.s200_spd_max =
        g_sensor_state.s200_spd_avg = s200_spd_raw;

        g_sensor_state.s200_dir_min =
        g_sensor_state.s200_dir_max =
        g_sensor_state.s200_dir_avg = s200_dir_raw;

        g_sensor_state.s200_heat_high =
        g_sensor_state.s200_heat_low  = s200_heat_raw;
    }

    xSemaphoreGive(g_sensor_state.mutex);

    Serial.printf("[live_fetch] Injected — temp=%.1f°C hum=%.0f%% "
                  "spd=%.3fm/s dir=%.0f° heat=%.3f°C\n",
                  temp_c, hum_pct, spd_ms, dir_deg, temp_c);
    return true;
}

// ---------------------------------------------------------------------------
// Task body
// ---------------------------------------------------------------------------

/**
 * @brief Live-fetch task.
 *
 * Blocks waiting for a task notification.  When either mode task switches to
 * LIVE it calls live_fetch_task_notify(); the fetch task then:
 *
 *   1. Waits until WiFi STA is connected (EventGroup bit).
 *   2. Calls geo_ip_get_location() once per new IP address.
 *   3. Fetches Open-Meteo and injects values.
 *   4. Sleeps for LIVE_FETCH_INTERVAL_S; on early notification repeats from 3
 *      (rate-limited: skip if last fetch was < LIVE_FETCH_INTERVAL_S ago).
 *   5. When neither sensor is in LIVE mode, blocks indefinitely.
 */
static void live_fetch_task(void *arg)
{
    EventGroupHandle_t eg = wifi_manager_get_event_group();

    for (;;) {
        // Wait indefinitely for a notification (from a mode task).
        // Also wakes after the fetch interval so we refresh on schedule.
        TickType_t interval_ticks = pdMS_TO_TICKS(LIVE_FETCH_INTERVAL_S * 1000UL);
        TickType_t elapsed = xTaskGetTickCount() - s_last_fetch_tick;
        TickType_t wait    = (s_last_fetch_tick == 0 || elapsed >= interval_ticks)
                             ? portMAX_DELAY
                             : (interval_ticks - elapsed);

        ulTaskNotifyTake(pdTRUE, wait);

        // Skip if no sensor needs live data.
        if (!any_sensor_live()) {
            s_last_fetch_tick = 0;  // Reset so next notification triggers immediately.
            continue;
        }

        // Rate-limit: skip if interval hasn't elapsed yet (can happen when the
        // mode task sends its 5 s safety notification before our timer fires).
        TickType_t now = xTaskGetTickCount();
        if (s_last_fetch_tick != 0 &&
            (now - s_last_fetch_tick) < interval_ticks)
        {
            // Recalculate a proper wait for the next cycle and go back to top.
            continue;
        }

        // Wait for WiFi STA connection.
        EventBits_t bits = xEventGroupWaitBits(eg,
                                               WIFI_EVT_STA_CONNECTED,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(30000));
        if (!(bits & WIFI_EVT_STA_CONNECTED)) {
            Serial.println("[live_fetch] WiFi not connected — skipping fetch");
            continue;
        }

        // Geo-IP refresh when the IP address changes (new connection).
        uint32_t current_ip = (uint32_t)WiFi.localIP();
        if (current_ip != s_last_ip && current_ip != 0) {
            s_last_ip = current_ip;
            float g_lat, g_lon;
            geo_ip_get_location(&g_lat, &g_lon);  // Updates NVS on success.
        }

        // Fetch and inject.  Only advance the tick on success so the rate
        // limiter and the UI progress bar correctly reflect the last *good* fetch.
        s_last_fetch_ok = fetch_and_inject();
        if (s_last_fetch_ok) {
            s_last_fetch_tick = xTaskGetTickCount();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void live_fetch_task_init(void)
{
    xTaskCreate(live_fetch_task, "live_fetch", LIVE_FETCH_STACK,
                nullptr, 2, &s_handle);
    // Task starts blocked in ulTaskNotifyTake — effectively suspended
    // until a sensor is switched to LIVE mode and live_fetch_task_notify()
    // is called.
}

void live_fetch_task_notify(void)
{
    if (s_handle) {
        xTaskNotifyGive(s_handle);
    }
}

int live_fetch_get_age_s(void)
{
    if (s_last_fetch_tick == 0) return -1;
    TickType_t elapsed_ticks = xTaskGetTickCount() - s_last_fetch_tick;
    int age_s = (int)(elapsed_ticks / portTICK_PERIOD_MS / 1000);
    return age_s;
}

bool live_fetch_was_ok(void)
{
    return s_last_fetch_ok;
}
