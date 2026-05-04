/**
 * @file s200_mode_task.cpp
 * @brief S200 sensor mode dispatcher — Phase 8.
 *
 * For SENSOR_MODE_MANUAL (Phase 8) the web POST handler writes
 * g_sensor_state directly under mutex; the modbus_slave_task reads
 * from g_sensor_state without any additional work here.
 *
 * SENSOR_MODE_LIVE (Phase 10) and SENSOR_MODE_REPLAY (Phase 11) stub
 * branches are present so future phases can extend this task in-place
 * without restructuring the dispatch logic.
 */

#include "s200_mode_task.h"
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

void s200_mode_task_notify(void)
{
    if (s_handle) {
        xTaskNotifyGive(s_handle);
    }
}

// ---------------------------------------------------------------------------
// Task body
// ---------------------------------------------------------------------------

/**
 * @brief S200 mode dispatcher task.
 *
 * Blocks on a task notification (5 s safety timeout) then reads s200_mode
 * from sensor_state and dispatches:
 *
 *   - MANUAL  — state is already up-to-date (POST handler wrote it); no-op.
 *   - LIVE    — Phase 10 stub.
 *   - REPLAY  — Phase 11 stub.
 *
 * @param arg  Unused.
 */
void s200_mode_task(void *arg)
{
    s_handle = xTaskGetCurrentTaskHandle();

    for (;;) {
        // Block until notified by the POST handler, or wake after 5 s to
        // catch any missed notification (e.g. on mode restore from NVS).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

        xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
        sensor_mode_t mode = g_sensor_state.s200_mode;
        xSemaphoreGive(g_sensor_state.mutex);

        switch (mode) {
        case SENSOR_MODE_MANUAL:
            // Phase 8: the web POST handler already wrote the value to
            // sensor_state under mutex and to NVS.  The modbus_slave_task
            // reads from sensor_state — nothing more needed here.
            break;

        case SENSOR_MODE_LIVE:
            // Phase 10: live_fetch_task will inject values from Open-Meteo.
            Serial.println("[s200_mode] LIVE — Phase 10 not yet implemented");
            break;

        case SENSOR_MODE_REPLAY:
            // Phase 11: replay_task will inject values from the CSV file.
            Serial.println("[s200_mode] REPLAY — Phase 11 not yet implemented");
            break;
        }
    }
}
