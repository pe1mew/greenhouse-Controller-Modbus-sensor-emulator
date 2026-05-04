/**
 * @file replay_task.cpp
 * @brief Timestamped CSV replay task — Phase 11.
 *
 * Plays back a SPIFFS CSV file row-by-row in wall-clock local time.
 * Before each row is applied, values are clamped to their physical sensor
 * ranges (design §11.1); any clamping event is printed to the serial log.
 *
 * Signal protocol (FreeRTOS task notifications):
 *   replay_task_start()  — sets s_start_req = true, calls xTaskNotifyGive
 *   replay_task_stop()   — sets s_stop_req  = true, calls xTaskNotifyGive
 *
 * The task body uses ulTaskNotifyTake(pdTRUE, ...) both to await a start
 * command (portMAX_DELAY) and as a 500 ms periodic wake-up during playback
 * so that stop requests are honoured within half a second.
 */

#include "replay_task.h"
#include "../util/csv_parser.h"
#include "../sensors/sensor_state.h"
#include "../config/nvs_config.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** @brief Stack size in bytes for the replay task. */
static constexpr uint32_t REPLAY_STACK = 4096;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static TaskHandle_t            s_handle    = nullptr;
static volatile replay_state_t s_state     = REPLAY_IDLE;
static volatile bool           s_start_req = false;
static volatile bool           s_stop_req  = false;
static volatile int            s_row_index = -1;

// ---------------------------------------------------------------------------
// Clamping helper
// ---------------------------------------------------------------------------

/**
 * @brief Convert @p v by @p scale, round, clamp to [@p lo, @p hi].
 *
 * @param v           Physical value.
 * @param scale       Multiplier (e.g. 10.0 for FG6485A, 1000.0 for S200).
 * @param lo          Minimum raw value (inclusive).
 * @param hi          Maximum raw value (inclusive).
 * @param clamped_out Set to @c true if the value was adjusted.
 * @return            Raw int32_t register value.
 */
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

/**
 * @brief Clamp and inject all present fields from @p row into g_sensor_state.
 *
 * Only sensors whose mode is @c SENSOR_MODE_REPLAY at the time of injection
 * are updated.
 */
static void inject_row(const csv_row_t *row)
{
    bool clamped = false;

    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);

    if (g_sensor_state.fg_mode == SENSOR_MODE_REPLAY) {
        if (row->has_fg_temp) {
            int32_t raw = to_raw(row->fg_temp, 10.0f,
                                  FG6485A_TEMP_RAW_MIN, FG6485A_TEMP_RAW_MAX,
                                  &clamped);
            if (clamped) {
                Serial.printf("[replay] fg_temp clamped: %.1f → %.1f °C\n",
                              row->fg_temp, raw / 10.0f);
            }
            g_sensor_state.fg_temperature = (int16_t)raw;
        }
        if (row->has_fg_hum) {
            int32_t raw = to_raw(row->fg_hum, 10.0f,
                                  (int32_t)FG6485A_HUM_RAW_MIN,
                                  (int32_t)FG6485A_HUM_RAW_MAX,
                                  &clamped);
            if (clamped) {
                Serial.printf("[replay] fg_hum clamped: %.1f → %.1f %%RH\n",
                              row->fg_hum, raw / 10.0f);
            }
            g_sensor_state.fg_humidity = (uint16_t)raw;
        }
    }

    if (g_sensor_state.s200_mode == SENSOR_MODE_REPLAY) {
        if (row->has_s200_spd) {
            int32_t raw = to_raw(row->s200_spd, 1000.0f,
                                  S200_SPD_RAW_MIN, S200_SPD_RAW_MAX,
                                  &clamped);
            if (clamped) {
                Serial.printf("[replay] s200_spd clamped: %.3f → %.3f m/s\n",
                              row->s200_spd, raw / 1000.0f);
            }
            g_sensor_state.s200_spd_min =
            g_sensor_state.s200_spd_max =
            g_sensor_state.s200_spd_avg = raw;
        }
        if (row->has_s200_dir) {
            int32_t raw = to_raw(row->s200_dir, 1000.0f,
                                  S200_DIR_RAW_MIN, S200_DIR_RAW_MAX,
                                  &clamped);
            if (clamped) {
                Serial.printf("[replay] s200_dir clamped: %.3f → %.3f °\n",
                              row->s200_dir, raw / 1000.0f);
            }
            g_sensor_state.s200_dir_min =
            g_sensor_state.s200_dir_max =
            g_sensor_state.s200_dir_avg = raw;
        }
        if (row->has_s200_heat) {
            int32_t raw = to_raw(row->s200_heat, 1000.0f,
                                  S200_HEAT_RAW_MIN, S200_HEAT_RAW_MAX,
                                  &clamped);
            if (clamped) {
                Serial.printf("[replay] s200_heat clamped: %.3f → %.3f °C\n",
                              row->s200_heat, raw / 1000.0f);
            }
            g_sensor_state.s200_heat_high =
            g_sensor_state.s200_heat_low  = raw;
        }
    }

    xSemaphoreGive(g_sensor_state.mutex);
}

// ---------------------------------------------------------------------------
// Task body
// ---------------------------------------------------------------------------

static void replay_task_body(void * /*arg*/)
{
    for (;;) {
        // ------------------------------------------------------------------
        // IDLE: wait for a start request.
        // ------------------------------------------------------------------
        s_state     = REPLAY_IDLE;
        s_row_index = -1;

        while (!s_start_req) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (s_stop_req) s_stop_req = false;  // discard stop when not running
        }
        s_start_req = false;
        s_stop_req  = false;

        // ------------------------------------------------------------------
        // Pre-flight: system clock must be plausibly set (> 2020-01-01).
        // ------------------------------------------------------------------
        time_t now = time(NULL);
        if (now < 1577836800LL) {
            Serial.println("[replay] Clock not set — cannot start");
            s_state = REPLAY_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));  // brief pause so the UI sees ERROR
            continue;
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
        // Seek to first row with timestamp >= now.
        // ------------------------------------------------------------------
        csv_row_t row;
        bool found = false;
        now = time(NULL);  // refresh after file open
        while (csv_next_row(csv, &row)) {
            if (row.has_ts) {
                struct tm ts_copy = row.ts;
                if (mktime(&ts_copy) >= now) {
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            Serial.println("[replay] No future rows in CSV");
            csv_close(csv);
            s_state = REPLAY_DONE;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ------------------------------------------------------------------
        // Main playback loop.
        // ------------------------------------------------------------------
        s_state = REPLAY_RUNNING;
        int row_num  = 0;
        bool stopped = false;

        do {
            // Check for stop request at the top of each row iteration.
            if (s_stop_req) {
                s_stop_req = false;
                stopped    = true;
                break;
            }

            // Wait for row timestamp, checking for stop every 500 ms.
            if (row.has_ts) {
                struct tm ts_copy = row.ts;
                time_t row_t = mktime(&ts_copy);
                while (time(NULL) < row_t) {
                    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) != 0) {
                        if (s_stop_req) {
                            s_stop_req = false;
                            stopped    = true;
                            break;
                        }
                    }
                }
            }

            if (stopped) break;

            // Inject row and advance row counter.
            s_row_index = row_num;
            inject_row(&row);
            Serial.printf("[replay] Row %d injected\n", row_num);
            row_num++;

        } while (csv_next_row(csv, &row));

        csv_close(csv);

        if (stopped) {
            Serial.println("[replay] Stopped by request");
            s_state     = REPLAY_IDLE;
            s_row_index = -1;
        } else {
            Serial.println("[replay] Playback complete (EOF)");
            s_state     = REPLAY_DONE;
            s_row_index = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));  // hold DONE visible briefly
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void replay_task_init(void)
{
    xTaskCreate(replay_task_body, "replay", REPLAY_STACK, nullptr,
                tskIDLE_PRIORITY + 1, &s_handle);
}

void replay_task_start(void)
{
    if (!s_handle) return;
    if (s_state == REPLAY_RUNNING) return;
    s_start_req = true;
    xTaskNotifyGive(s_handle);
}

void replay_task_stop(void)
{
    if (!s_handle) return;
    s_stop_req = true;
    xTaskNotifyGive(s_handle);  // wake task if it's sleeping
}

replay_state_t replay_task_get_state(void)
{
    return s_state;
}

int replay_task_get_row(void)
{
    return s_row_index;
}
