/**
 * @file replay_task.h
 * @brief Relative-time CSV replay task public API — Phase 13.
 *
 * Plays back a SPIFFS CSV file row-by-row using a relative elapsed-time
 * timer that starts at zero when the user presses Start.  No NTP required.
 *
 * Expected CSV format (header + data rows, max REPLAY_MAX_ROWS = 2900):
 * @code
 * timestamp,fg_temp,fg_hum,s200_spd,s200_dir,s200_heat
 * 00:00:00,25.0,60,,180,
 * 00:01:30,26.5,58,3.2,,
 * @endcode
 * Timestamp is a relative offset (HH:MM:SS) from the moment Start is pressed.
 * Sensor columns are optional; an empty cell means "do not change that value".
 *
 * Transport controls:
 *   cmd_start  — open file, build index, begin RUNNING (timer = 0)
 *   cmd_stop   — abort from any state, return to IDLE
 *   cmd_pause  — freeze timer; enables Next/Prev navigation
 *   cmd_play   — resume timer from current elapsed_s
 *   cmd_next   — (PAUSED only) advance one row, inject, set elapsed_s
 *   cmd_prev   — (PAUSED only) retreat one row, inject, set elapsed_s
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum data rows in a replay CSV file. */
#define REPLAY_MAX_ROWS  2900

/** @brief Operating states of the replay task. */
typedef enum {
    REPLAY_IDLE    = 0, /**< Waiting for a start command. */
    REPLAY_RUNNING = 1, /**< Actively playing back CSV rows by elapsed time. */
    REPLAY_PAUSED  = 2, /**< Timer frozen; Next/Prev navigation active. */
    REPLAY_DONE    = 3, /**< Reached end-of-file normally. */
    REPLAY_ERROR   = 4, /**< File-open or parse error. */
} replay_state_t;

/**
 * @brief One entry in the 3-row event context window.
 *
 * A @c valid = @c false entry means there is no row at that position
 * (e.g. prev at row 0, or next at last row).
 */
typedef struct {
    bool     valid;
    int      row;
    uint32_t ts_s;
    float    fg_temp;   bool has_fg_temp;
    float    fg_hum;    bool has_fg_hum;
    float    s200_spd;  bool has_s200_spd;
    float    s200_dir;  bool has_s200_dir;
    float    s200_heat; bool has_s200_heat;
} replay_window_entry_t;

/**
 * @brief Create the replay FreeRTOS task (starts in REPLAY_IDLE state).
 *
 * Must be called once from setup() after SPIFFS and NVS are ready.
 */
void replay_task_init(void);

/** @name Transport commands — safe to call from any FreeRTOS task context. @{ */
void replay_task_cmd_start(void);
void replay_task_cmd_stop(void);
void replay_task_cmd_pause(void);
void replay_task_cmd_play(void);
void replay_task_cmd_next(void);
void replay_task_cmd_prev(void);
/** @} */

/** @brief Return the current replay operating state. */
replay_state_t replay_task_get_state(void);

/** @brief Return the 0-based index of the row most recently injected or
 *         navigated to.  Returns -1 when no row has been processed yet. */
int replay_task_get_row(void);

/** @brief Return the elapsed timer value in seconds. */
uint32_t replay_task_get_elapsed_s(void);

/** @brief Return the total number of data rows in the loaded CSV (0 when IDLE). */
int replay_task_get_row_count(void);

/**
 * @brief Return true and atomically clear the window-dirty flag if the
 *        event window has changed since the last call.
 *
 * Intended for ws_push_task: call once per push cycle; if true, follow up
 * with replay_task_get_window() to obtain the new window data.
 */
bool replay_task_consume_window_dirty(void);

/**
 * @brief Copy the current 3-row event context window into the caller's structs.
 *
 * Thread-safe: protected internally by a mutex.  All three out-pointers
 * must be non-null.
 */
void replay_task_get_window(replay_window_entry_t *prev_out,
                            replay_window_entry_t *curr_out,
                            replay_window_entry_t *next_out);

#ifdef __cplusplus
}
#endif
