/**
 * @file s200_mode_task.h
 * @brief S200 sensor mode dispatcher — Phase 8.
 *
 * Provides the FreeRTOS task function and notification API for the S200
 * mode dispatcher.  The task reads @c g_sensor_state.s200_mode under mutex
 * and acts on mode transitions:
 *
 *   - @c SENSOR_MODE_MANUAL  — web POST handler writes @c sensor_state
 *                              directly; no further action needed in this task.
 *   - @c SENSOR_MODE_LIVE    — live_fetch_task injects values (Phase 10).
 *   - @c SENSOR_MODE_REPLAY  — replay_task injects values (Phase 11).
 *
 * The web POST handler calls @ref s200_mode_task_notify() after every
 * write to @c g_sensor_state so the task can react to mode changes.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FreeRTOS task function for the S200 mode dispatcher.
 *
 * Start with @c xTaskCreate() at low priority (@c tskIDLE_PRIORITY + 1).
 * Runs for the lifetime of the application.
 *
 * @param arg  Unused.
 */
void s200_mode_task(void *arg);

/**
 * @brief Wake the S200 mode task after a mode or value change.
 *
 * Called by the web POST handler after writing a new mode or manual value
 * to @c g_sensor_state.  Safe to call from any task context.
 */
void s200_mode_task_notify(void);

#ifdef __cplusplus
}
#endif
