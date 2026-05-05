/**
 * @file replay_task.cpp
 * @brief Relative-time CSV replay task â€” Phase 13.
 *
 * Redesign from Phase 11:
 *  - Timestamps are relative offsets (HH:MM:SS) â€” no NTP required.
 *  - In-memory row index (file_offset + ts_s per row) for O(1) seeks.
 *  - Full transport: Start / Stop / Pause / Play / Next / Prev.
 *  - FreeRTOS command queue replaces volatile boolean flags.
 *  - 3-row event window shared with ws_push_task via mutex + dirty flag.
 */

#include "replay_task.h"
#include "../util/csv_parser.h"
#include "../sensors/sensor_state.h"
#include "../config/nvs_config.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static constexpr uint32_t REPLAY_STACK    = 4096;
static constexpr uint32_t CMD_QUEUE_DEPTH = 8;

// ---------------------------------------------------------------------------
// Command tokens
// ---------------------------------------------------------------------------

typedef enum : uint8_t {
    CMD_START = 0,
    CMD_STOP,
    CMD_PAUSE,
    CMD_PLAY,
    CMD_NEXT,
    CMD_PREV,
} replay_cmd_t;

// ---------------------------------------------------------------------------
// Row index entry
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t file_offset;   /**< Byte offset in the SPIFFS file. */
    uint32_t ts_s;          /**< Timestamp in seconds. */
} replay_index_entry_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static TaskHandle_t          s_handle      = nullptr;
static QueueHandle_t         s_cmd_queue   = nullptr;
static SemaphoreHandle_t     s_win_mutex   = nullptr;

static volatile replay_state_t s_state     = REPLAY_IDLE;
static volatile int            s_row_index = -1;
static volatile uint32_t       s_elapsed_s = 0;
static volatile int            s_row_count = 0;

static replay_index_entry_t   *s_index     = nullptr;

// Window data (protected by s_win_mutex).
static replay_window_entry_t   s_win_prev;
static replay_window_entry_t   s_win_curr;
static replay_window_entry_t   s_win_next;
static volatile bool           s_window_dirty = false;

// ---------------------------------------------------------------------------
// Clamping helper
// ---------------------------------------------------------------------------

static inline int32_t to_raw(float v, float scale,
                              int32_t lo, int32_t hi, bool *clamped_out)
{
    int32_t raw = (int32_t)roundf(v * scale);
    int32_t cr  = raw < lo ? lo : (raw > hi ? hi : raw);
    *clamped_out = (cr != raw);
    return cr;
}

// ---------------------------------------------------------------------------
// Row injection
// ---------------------------------------------------------------------------

static void inject_row(const csv_row_t *row)
{
    bool clamped = false;

    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);

    if (g_sensor_state.fg_mode == SENSOR_MODE_REPLAY) {
        if (row->has_fg_temp) {
            int32_t raw = to_raw(row->fg_temp, 10.0f,
                                  FG6485A_TEMP_RAW_MIN, FG6485A_TEMP_RAW_MAX,
                                  &clamped);
            if (clamped) Serial.printf("[replay] fg_temp clamped: %.1f\xc2\xb0""C\n",
                                       raw / 10.0f);
            g_sensor_state.fg_temperature = (int16_t)raw;
        }
        if (row->has_fg_hum) {
            int32_t raw = to_raw(row->fg_hum, 10.0f,
                                  (int32_t)FG6485A_HUM_RAW_MIN,
                                  (int32_t)FG6485A_HUM_RAW_MAX,
                                  &clamped);
            if (clamped) Serial.printf("[replay] fg_hum clamped: %.1f %%RH\n",
                                       raw / 10.0f);
            g_sensor_state.fg_humidity = (uint16_t)raw;
        }
    }

    if (g_sensor_state.s200_mode == SENSOR_MODE_REPLAY) {
        if (row->has_s200_spd) {
            int32_t raw = to_raw(row->s200_spd, 1000.0f,
                                  S200_SPD_RAW_MIN, S200_SPD_RAW_MAX, &clamped);
            if (clamped) Serial.printf("[replay] s200_spd clamped: %.3f m/s\n",
                                       raw / 1000.0f);
            g_sensor_state.s200_spd_min =
            g_sensor_state.s200_spd_max =
            g_sensor_state.s200_spd_avg = raw;
        }
        if (row->has_s200_dir) {
            int32_t raw = to_raw(row->s200_dir, 1000.0f,
                                  S200_DIR_RAW_MIN, S200_DIR_RAW_MAX, &clamped);
            if (clamped) Serial.printf("[replay] s200_dir clamped: %.3f\xc2\xb0\n",
                                       raw / 1000.0f);
            g_sensor_state.s200_dir_min =
            g_sensor_state.s200_dir_max =
            g_sensor_state.s200_dir_avg = raw;
        }
        if (row->has_s200_heat) {
            int32_t raw = to_raw(row->s200_heat, 1000.0f,
                                  S200_HEAT_RAW_MIN, S200_HEAT_RAW_MAX, &clamped);
            if (clamped) Serial.printf("[replay] s200_heat clamped: %.3f\xc2\xb0""C\n",
                                       raw / 1000.0f);
            g_sensor_state.s200_heat_high =
            g_sensor_state.s200_heat_low  = raw;
        }
    }

    xSemaphoreGive(g_sensor_state.mutex);
}

// ---------------------------------------------------------------------------
// Window helpers
// ---------------------------------------------------------------------------

static void fill_win_entry(replay_window_entry_t *e, int row,
                            const csv_row_t *r)
{
    e->valid        = true;
    e->row          = row;
    e->ts_s         = r->has_ts ? r->ts_s : s_index[row].ts_s;
    e->fg_temp      = r->fg_temp;  e->has_fg_temp  = r->has_fg_temp;
    e->fg_hum       = r->fg_hum;   e->has_fg_hum   = r->has_fg_hum;
    e->s200_spd     = r->s200_spd; e->has_s200_spd = r->has_s200_spd;
    e->s200_dir     = r->s200_dir; e->has_s200_dir = r->has_s200_dir;
    e->s200_heat    = r->s200_heat;e->has_s200_heat= r->has_s200_heat;
}

/** Update the shared window; called from replay task only. */
static void update_window(csv_parser_t *csv, int curr_row,
                           const csv_row_t *curr_data)
{
    replay_window_entry_t tmp_prev = {}, tmp_curr = {}, tmp_next = {};

    if (curr_row > 0) {
        csv_row_t r;
        if (csv_seek(csv, (size_t)s_index[curr_row - 1].file_offset, &r)) {
            fill_win_entry(&tmp_prev, curr_row - 1, &r);
        }
    }

    fill_win_entry(&tmp_curr, curr_row, curr_data);

    if (curr_row + 1 < s_row_count) {
        csv_row_t r;
        if (csv_seek(csv, (size_t)s_index[curr_row + 1].file_offset, &r)) {
            fill_win_entry(&tmp_next, curr_row + 1, &r);
        }
    }

    xSemaphoreTake(s_win_mutex, portMAX_DELAY);
    s_win_prev     = tmp_prev;
    s_win_curr     = tmp_curr;
    s_win_next     = tmp_next;
    s_window_dirty = true;
    xSemaphoreGive(s_win_mutex);
}

// ---------------------------------------------------------------------------
// Command dispatch (called within the task body)
// ---------------------------------------------------------------------------

static void dispatch_cmd(replay_cmd_t cmd, bool *stopped_out,
                         int *next_inject, csv_parser_t *csv)
{
    switch (cmd) {
        case CMD_STOP:
            *stopped_out = true;
            break;

        case CMD_PAUSE:
            if (s_state == REPLAY_RUNNING) {
                s_state = REPLAY_PAUSED;
                Serial.println("[replay] Paused");
            }
            break;

        case CMD_PLAY:
            if (s_state == REPLAY_PAUSED) {
                s_state = REPLAY_RUNNING;
                Serial.println("[replay] Resumed");
            }
            break;

        case CMD_NEXT:
            if (s_state == REPLAY_PAUSED && s_row_index + 1 < s_row_count) {
                int nxt = s_row_index + 1;
                csv_row_t r;
                if (csv && csv_seek(csv, (size_t)s_index[nxt].file_offset, &r)) {
                    inject_row(&r);
                    update_window(csv, nxt, &r);
                }
                s_row_index  = nxt;
                s_elapsed_s  = s_index[nxt].ts_s;
                *next_inject = nxt + 1;
                Serial.printf("[replay] Next -> row %d\n", nxt);
            }
            break;

        case CMD_PREV:
            if (s_state == REPLAY_PAUSED && s_row_index > 0) {
                int prv = s_row_index - 1;
                csv_row_t r;
                if (csv && csv_seek(csv, (size_t)s_index[prv].file_offset, &r)) {
                    inject_row(&r);
                    update_window(csv, prv, &r);
                }
                s_row_index  = prv;
                s_elapsed_s  = s_index[prv].ts_s;
                *next_inject = prv + 1;
                Serial.printf("[replay] Prev -> row %d\n", prv);
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Task body
// ---------------------------------------------------------------------------

static void replay_task_body(void * /*arg*/)
{
    for (;;) {
        // ------------------------------------------------------------------
        // IDLE: reset all state, wait for CMD_START.
        // ------------------------------------------------------------------
        s_state        = REPLAY_IDLE;
        s_row_index    = -1;
        s_elapsed_s    = 0;
        s_row_count    = 0;
        s_window_dirty = false;
        xSemaphoreTake(s_win_mutex, portMAX_DELAY);
        memset(&s_win_prev, 0, sizeof(s_win_prev));
        memset(&s_win_curr, 0, sizeof(s_win_curr));
        memset(&s_win_next, 0, sizeof(s_win_next));
        xSemaphoreGive(s_win_mutex);

        {
            replay_cmd_t cmd;
            while (true) {
                xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY);
                if (cmd == CMD_START) break;
                // Discard all other commands while IDLE.
            }
        }

        // ------------------------------------------------------------------
        // Open CSV file.
        // ------------------------------------------------------------------
        char path[NVS_STR_MAX_PATH];
        nvs_cfg_get_str(NVS_KEY_REPLAY_FILE, path, sizeof(path), "");
        if (path[0] == '\0') {
            Serial.println("[replay] No CSV file configured");
            s_state = REPLAY_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        csv_parser_t *csv = csv_open(path);
        if (!csv) {
            Serial.printf("[replay] Cannot open %s\n", path);
            s_state = REPLAY_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ------------------------------------------------------------------
        // Build row index.
        // ------------------------------------------------------------------
        if (s_index) { free(s_index); s_index = nullptr; }
        s_index = (replay_index_entry_t *)malloc(
                        REPLAY_MAX_ROWS * sizeof(replay_index_entry_t));
        if (!s_index) {
            Serial.println("[replay] OOM building row index");
            csv_close(csv);
            s_state = REPLAY_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int count = 0;
        csv_row_t row;
        while (csv_next_row(csv, &row) && count < REPLAY_MAX_ROWS) {
            s_index[count].file_offset = (uint32_t)csv_tell(csv);
            s_index[count].ts_s        = row.has_ts ? row.ts_s : 0u;
            count++;
        }
        s_row_count = count;

        if (count == 0) {
            Serial.println("[replay] CSV has no data rows");
            csv_close(csv);
            free(s_index); s_index = nullptr;
            s_state = REPLAY_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        Serial.printf("[replay] Index built: %d rows\n", count);

        // ------------------------------------------------------------------
        // Begin playback.
        // ------------------------------------------------------------------
        s_state     = REPLAY_RUNNING;
        s_elapsed_s = 0;
        s_row_index = -1;
        int  next_inject = 0;
        bool stopped     = false;

        // Inject any rows at ts_s == 0 immediately.
        while (next_inject < s_row_count &&
               s_index[next_inject].ts_s == 0) {
            csv_row_t r;
            if (csv_seek(csv, (size_t)s_index[next_inject].file_offset, &r)) {
                inject_row(&r);
                s_row_index = next_inject;
                update_window(csv, next_inject, &r);
                Serial.printf("[replay] Row %d injected (t=0)\n", next_inject);
            }
            next_inject++;
        }

        // ------------------------------------------------------------------
        // Main playback loop.
        // ------------------------------------------------------------------
        for (;;) {
            if (s_state == REPLAY_RUNNING) {
                // Wait up to 1 s for a command, tracking actual elapsed time.
                TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
                replay_cmd_t cmd;
                while (s_state == REPLAY_RUNNING && !stopped) {
                    TickType_t now     = xTaskGetTickCount();
                    TickType_t remain  = deadline - now;
                    // remain wraps if deadline passed; cap to 0.
                    if (remain > pdMS_TO_TICKS(1000)) remain = 0;
                    if (remain == 0) break;
                    if (xQueueReceive(s_cmd_queue, &cmd, remain) == pdTRUE) {
                        dispatch_cmd(cmd, &stopped, &next_inject, csv);
                    }
                }
                if (stopped) break;

                // Advance timer only if still RUNNING after processing cmds.
                if (s_state == REPLAY_RUNNING) {
                    s_elapsed_s++;

                    // Inject all rows whose ts_s <= elapsed_s.
                    while (next_inject < s_row_count &&
                           s_index[next_inject].ts_s <= s_elapsed_s) {
                        csv_row_t r;
                        if (csv_seek(csv,
                                (size_t)s_index[next_inject].file_offset, &r)) {
                            inject_row(&r);
                            s_row_index = next_inject;
                            update_window(csv, next_inject, &r);
                            Serial.printf("[replay] Row %d injected (t=%us)\n",
                                          next_inject, (unsigned)s_elapsed_s);
                        }
                        next_inject++;
                    }

                    // Check for end of file.
                    if (next_inject >= s_row_count) {
                        Serial.println("[replay] Playback complete (EOF)");
                        break;
                    }
                }

            } else {
                // PAUSED: block until a command arrives.
                replay_cmd_t cmd;
                xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY);
                dispatch_cmd(cmd, &stopped, &next_inject, csv);
                if (stopped) break;
            }
        }

        csv_close(csv);
        csv = nullptr;
        if (s_index) { free(s_index); s_index = nullptr; }

        if (stopped) {
            Serial.println("[replay] Stopped");
            // s_state reset to IDLE at top of outer for(;;).
        } else {
            s_state = REPLAY_DONE;
            vTaskDelay(pdMS_TO_TICKS(2000));  // hold DONE briefly
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void replay_task_init(void)
{
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(uint8_t));
    s_win_mutex = xSemaphoreCreateMutex();
    xTaskCreate(replay_task_body, "replay", REPLAY_STACK, nullptr,
                tskIDLE_PRIORITY + 1, &s_handle);
}

static void send_cmd(replay_cmd_t cmd)
{
    if (!s_cmd_queue) return;
    uint8_t c = (uint8_t)cmd;
    xQueueSend(s_cmd_queue, &c, 0);
}

void replay_task_cmd_start(void) { send_cmd(CMD_START); }
void replay_task_cmd_stop(void)  { send_cmd(CMD_STOP);  }
void replay_task_cmd_pause(void) { send_cmd(CMD_PAUSE); }
void replay_task_cmd_play(void)  { send_cmd(CMD_PLAY);  }
void replay_task_cmd_next(void)  { send_cmd(CMD_NEXT);  }
void replay_task_cmd_prev(void)  { send_cmd(CMD_PREV);  }

replay_state_t replay_task_get_state(void)    { return s_state;     }
int            replay_task_get_row(void)       { return s_row_index; }
uint32_t       replay_task_get_elapsed_s(void) { return s_elapsed_s; }
int            replay_task_get_row_count(void) { return s_row_count; }

bool replay_task_consume_window_dirty(void)
{
    if (!s_window_dirty) return false;
    xSemaphoreTake(s_win_mutex, portMAX_DELAY);
    bool dirty     = s_window_dirty;
    s_window_dirty = false;
    xSemaphoreGive(s_win_mutex);
    return dirty;
}

void replay_task_get_window(replay_window_entry_t *prev_out,
                            replay_window_entry_t *curr_out,
                            replay_window_entry_t *next_out)
{
    xSemaphoreTake(s_win_mutex, portMAX_DELAY);
    *prev_out = s_win_prev;
    *curr_out = s_win_curr;
    *next_out = s_win_next;
    xSemaphoreGive(s_win_mutex);
}
