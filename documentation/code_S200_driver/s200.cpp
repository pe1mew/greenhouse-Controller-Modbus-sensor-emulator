/**
 * @file s200.cpp
 * @brief SenseCAP ONE V2 S200 wind sensor driver implementation.
 *
 * All raw register values from the S200 are int32 × 1000 of the actual
 * engineering value, encoded as two consecutive uint16 registers in big-endian
 * word order (high word at the lower address).  The decode_int32() helper
 * reconstructs the signed 32-bit value; read functions then divide by 1000.0f.
 *
 * Two FC04 requests are issued per measurement call:
 *   1. Registers 0x0008–0x0013 (12 registers): wind direction + wind speed
 *   2. Registers 0x001C–0x001D (2 registers):  heating temperature
 *
 * Depends on LIB-6 (modbus_rtu) for all bus transactions.
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#ifndef NATIVE_TEST
  #include <Arduino.h>
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
  #include "freertos/task.h"
#endif

#include "s200.h"
#include "modbus_rtu.h"

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * Map a modbus_status_t to the coarser s200_status_t.
 * MODBUS_ERR_PARAM passes through as S200_ERR_PARAM; all other errors
 * collapse to S200_ERR_COMM.
 */
static s200_status_t map_status(modbus_status_t s)
{
    if (s == MODBUS_OK)         return S200_OK;
    if (s == MODBUS_ERR_PARAM)  return S200_ERR_PARAM;
    return S200_ERR_COMM;
}

/**
 * Decode one int32 from two consecutive uint16 registers (big-endian word order).
 * regs[0] holds the high 16 bits; regs[1] holds the low 16 bits.
 */
static int32_t decode_int32(const uint16_t *regs)
{
    return (int32_t)(((uint32_t)regs[0] << 16u) | (uint32_t)regs[1]);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

s200_status_t s200_read_measurements(uint8_t slave_addr, s200_measurement_t *out)
{
    if (slave_addr == 0u || out == nullptr) {
        return S200_ERR_PARAM;
    }

    /* --- FC04 read 1: wind direction and speed (0x0008–0x0013, 12 registers) ---
     *   regs[0–1]   = min wind direction  (int32 × 1000, degrees)
     *   regs[2–3]   = max wind direction
     *   regs[4–5]   = avg wind direction
     *   regs[6–7]   = min wind speed      (int32 × 1000, m/s)
     *   regs[8–9]   = max wind speed
     *   regs[10–11] = avg wind speed
     */
    uint16_t regs[12];
    modbus_status_t s = modbus_read_input_registers(slave_addr,
                                                     S200_REG_WIND_DIR_MIN,
                                                     12u, regs);
    if (s != MODBUS_OK) {
        return map_status(s);
    }

    out->wind_dir_min_deg  = decode_int32(&regs[0])  / 1000.0f;
    out->wind_dir_max_deg  = decode_int32(&regs[2])  / 1000.0f;
    out->wind_dir_avg_deg  = decode_int32(&regs[4])  / 1000.0f;
    out->wind_speed_min_ms = decode_int32(&regs[6])  / 1000.0f;
    out->wind_speed_max_ms = decode_int32(&regs[8])  / 1000.0f;
    out->wind_speed_avg_ms = decode_int32(&regs[10]) / 1000.0f;

    /* --- FC04 read 2: heating temperature (0x001C–0x001D, 2 registers) ---
     *   hregs[0–1] = heating temperature (int32 × 1000, °C)
     */
    uint16_t hregs[2];
    s = modbus_read_input_registers(slave_addr,
                                     S200_REG_HEATING_TEMP,
                                     2u, hregs);
    if (s != MODBUS_OK) {
        return map_status(s);
    }

    out->heating_temperature_c = decode_int32(&hregs[0]) / 1000.0f;
    return S200_OK;
}

/* ---------------------------------------------------------------------------
 * FreeRTOS polling task
 * --------------------------------------------------------------------------- */

#ifndef NATIVE_TEST
void s200_task(void *pvParameters)
{
    s200_task_param_t *param = static_cast<s200_task_param_t *>(pvParameters);

    for (;;) {
        s200_measurement_t local;
        s200_status_t st = s200_read_measurements(param->slave_addr, &local);

        /* Update shared data under the caller-provided mutex */
        if (xSemaphoreTake(param->mutex, pdMS_TO_TICKS(10u)) == pdTRUE) {
            *param->out_data   = local;
            *param->out_status = st;
            xSemaphoreGive(param->mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(param->poll_interval_ms));
    }
    /* unreachable — task deleted externally with vTaskDelete() if needed */
}
#endif /* NATIVE_TEST */
