/**
 * @file fg6485a_mode_task.h
 * @brief FG6485A sensor mode dispatcher — Phase 8.
 *
 * Provides the FreeRTOS task function and notification API for the FG6485A
 * mode dispatcher.  The task reads @c g_sensor_state.fg_mode under mutex and
 * acts on mode transitions:
 *
 *   - @c SENSOR_MODE_MANUAL  — web POST handler writes @c sensor_state
 *                              directly; no further action needed in this task.
 *   - @c SENSOR_MODE_LIVE    — live_fetch_task injects values (Phase 10).
 *   - @c SENSOR_MODE_REPLAY  — replay_task injects values (Phase 11).
 *
 * The web POST handler calls @ref fg6485a_mode_task_notify() after every
 * write to @c g_sensor_state so the task can react to mode changes.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FreeRTOS task function for the FG6485A mode dispatcher.
 *
 * Start with @c xTaskCreate() at low priority (@c tskIDLE_PRIORITY + 1).
 * Runs for the lifetime of the application.
 *
 * @param arg  Unused.
 */
void fg6485a_mode_task(void *arg);

/**
 * @brief Wake the FG6485A mode task after a mode or value change.
 *
 * Called by the web POST handler after writing a new mode or manual value
 * to @c g_sensor_state.  Safe to call from any task context.
 */
void fg6485a_mode_task_notify(void);

#ifdef __cplusplus
}
#endif
