/**
 * @file fg6485a_mode_task.cpp
 * @brief FG6485A sensor mode dispatcher — Phase 8.
 *
 * For SENSOR_MODE_MANUAL (Phase 8) the web POST handler writes
 * g_sensor_state directly under mutex; the modbus_slave_task reads
 * from g_sensor_state without any additional work here.
 *
 * SENSOR_MODE_LIVE (Phase 10) and SENSOR_MODE_REPLAY (Phase 11) stub
 * branches are present so future phases can extend this task in-place
 * without restructuring the dispatch logic.
 */

#include "fg6485a_mode_task.h"
#include "live_fetch_task.h"
#include "../sensors/sensor_state.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

/** @cond INTERNAL */
static TaskHandle_t s_handle = nullptr;
/** @endcond */

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void fg6485a_mode_task_notify(void)
{
    if (s_handle) {
        xTaskNotifyGive(s_handle);
    }
}

// ---------------------------------------------------------------------------
// Task body
// ---------------------------------------------------------------------------

/**
 * @brief FG6485A mode dispatcher task.
 *
 * Blocks on a task notification (5 s safety timeout) then reads fg_mode
 * from sensor_state and dispatches:
 *
 *   - MANUAL  — state is already up-to-date (POST handler wrote it); no-op.
 *   - LIVE    — Phase 10 stub.
 *   - REPLAY  — Phase 11 stub.
 *
 * @param arg  Unused.
 */
void fg6485a_mode_task(void *arg)
{
    s_handle = xTaskGetCurrentTaskHandle();

    for (;;) {
        // Block until notified by the POST handler, or wake after 5 s to
        // catch any missed notification (e.g. on mode restore from NVS).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

        xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
        sensor_mode_t mode = g_sensor_state.fg_mode;
        xSemaphoreGive(g_sensor_state.mutex);

        switch (mode) {
        case SENSOR_MODE_MANUAL:
            // Phase 8: the web POST handler already wrote the value to
            // sensor_state under mutex and to NVS.  The modbus_slave_task
            // reads from sensor_state — nothing more needed here.
            break;

        case SENSOR_MODE_LIVE:
            // Phase 10: wake the live_fetch_task to fetch from Open-Meteo.
            live_fetch_task_notify();
            break;

        case SENSOR_MODE_REPLAY:
            // Phase 11: replay_task will inject values from the CSV file.
            Serial.println("[fg6485a_mode] REPLAY — Phase 11 not yet implemented");
            break;

        case SENSOR_MODE_REST:
            // REST mode: values are injected by POST /api/data in web_server;
            // nothing to do here.
            break;
        }
    }
}
