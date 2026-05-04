/**
 * @file replay_task.h
 * @brief Timestamped CSV replay task public API — Phase 11.
 *
 * Manages playback of a CSV file stored on SPIFFS.  When started, the task
 * seeks to the first row whose timestamp is ≥ the current local time, then
 * advances through rows in real time, injecting clamped sensor values into
 * @ref g_sensor_state for every sensor currently in @c SENSOR_MODE_REPLAY.
 *
 * Expected CSV format — header row followed by data rows:
 * @code
 * timestamp,fg_temp,fg_hum,s200_spd,s200_dir,s200_heat
 * 2026-05-04T14:00:00,18.5,65,3.2,270,12.5
 * 2026-05-04T14:01:00,18.6,64,3.5,275,12.6
 * @endcode
 * All sensor columns are optional; absent columns are not injected.
 *
 * Control flow:
 * -  The task starts in @c REPLAY_IDLE state; call @c replay_task_start()
 *    to begin playback.
 * -  Call @c replay_task_stop() at any time to abort.
 * -  When the file is exhausted the task moves to @c REPLAY_DONE and then
 *    back to @c REPLAY_IDLE automatically.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Operating states of the replay task. */
typedef enum {
    REPLAY_IDLE    = 0, /**< Waiting for a start command. */
    REPLAY_RUNNING = 1, /**< Actively playing back CSV rows. */
    REPLAY_DONE    = 2, /**< Reached end-of-file normally. */
    REPLAY_ERROR   = 3, /**< Pre-flight or file-open error. */
} replay_state_t;

/**
 * @brief Create the replay FreeRTOS task (starts in @c REPLAY_IDLE state).
 *
 * Must be called once from @c setup() after SPIFFS and NVS are ready.
 */
void replay_task_init(void);

/**
 * @brief Request playback to start from the current local time.
 *
 * Safe to call from any FreeRTOS task context.  Has no effect if the task
 * is already @c REPLAY_RUNNING.
 */
void replay_task_start(void);

/**
 * @brief Request playback to stop.
 *
 * Safe to call from any FreeRTOS task context.  Returns immediately; the
 * task transitions to @c REPLAY_IDLE asynchronously (within ~500 ms).
 */
void replay_task_stop(void);

/**
 * @brief Return the current replay operating state.
 */
replay_state_t replay_task_get_state(void);

/**
 * @brief Return the 0-based index of the row currently being served.
 *
 * Returns @c -1 when not running.
 */
int replay_task_get_row(void);

#ifdef __cplusplus
}
#endif
