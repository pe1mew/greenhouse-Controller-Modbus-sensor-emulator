/**
 * @file live_fetch_task.h
 * @brief Open-Meteo weather fetch task — Phase 10.
 *
 * A single FreeRTOS task that periodically queries the Open-Meteo public API
 * and injects the result into @ref g_sensor_state for any sensor currently in
 * @c SENSOR_MODE_LIVE.
 *
 * The task is started in a blocked state; it begins fetching only when
 * notified by a mode dispatcher (fg6485a_mode_task or s200_mode_task) after
 * either sensor is switched to LIVE mode.  When neither sensor is in LIVE
 * mode the task blocks indefinitely, consuming no CPU.
 *
 * Rate limiting: at most one API call per @c LIVE_FETCH_INTERVAL_S seconds
 * (default 87 s ≈ 1000 calls/day, within the Open-Meteo free tier).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and register the live-fetch FreeRTOS task.
 *
 * Creates the task in a blocked state.  Must be called once from @c setup()
 * after @c wifi_manager_init().
 */
void live_fetch_task_init(void);

/**
 * @brief Wake the live-fetch task to trigger an immediate fetch cycle.
 *
 * Called by fg6485a_mode_task and s200_mode_task whenever they observe
 * @c SENSOR_MODE_LIVE.  Safe to call from any task context.
 */
void live_fetch_task_notify(void);

/**
 * @brief Return seconds since the last successful Open-Meteo fetch.
 *
 * @return Age in seconds (0 = just fetched), or -1 if no fetch has occurred
 *         yet in the current boot.  Used by the web server to populate the
 *         live_fetch_age field in the WebSocket status JSON.
 */
int live_fetch_get_age_s(void);

/**
 * @brief Return true if the most recent fetch attempt succeeded.
 *
 * Returns false both before the first attempt and after any failed attempt.
 * Used by the web server to populate the live_fetch_ok field in the
 * WebSocket status JSON so the UI can display a ✓ / ✗ indicator.
 */
bool live_fetch_was_ok(void);

#ifdef __cplusplus
}
#endif
