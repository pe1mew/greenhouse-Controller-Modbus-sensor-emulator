/**
 * @file fg6485a.cpp
 * @brief FG6485A Humidity and Temperature Transmitter driver implementation.
 *
 * All raw register values from the FG6485A are integers × 10 of the actual
 * engineering value.  Read functions divide by 10.0f before returning; write
 * functions multiply the float argument by 10 and cast to int16_t.
 *
 * Depends on LIB-6 (modbus_rtu) for all bus transactions.
 * FC03 is used for reads; FC16 is used for writes.
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

#include "fg6485a.h"
#include "modbus_rtu.h"

/* ---------------------------------------------------------------------------
 * Internal helper
 * --------------------------------------------------------------------------- */

/**
 * Map a modbus_status_t to the coarser fg6485a_status_t.
 * MODBUS_ERR_PARAM passes through as FG6485A_ERR_PARAM; all other errors
 * collapse to FG6485A_ERR_COMM.
 */
static fg6485a_status_t map_status(modbus_status_t s)
{
    if (s == MODBUS_OK)         return FG6485A_OK;
    if (s == MODBUS_ERR_PARAM)  return FG6485A_ERR_PARAM;
    return FG6485A_ERR_COMM;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

fg6485a_status_t fg6485a_read_measurements(uint8_t slave_addr,
                                            fg6485a_measurement_t *out)
{
    if (slave_addr == 0u || out == nullptr) {
        return FG6485A_ERR_PARAM;
    }

    /* Read 2 registers starting at 0x0000:
     *   regs[0] = Humidity    (unsigned raw × 10)
     *   regs[1] = Temperature (signed   raw × 10)
     */
    uint16_t regs[2];
    modbus_status_t s = modbus_read_holding_registers(slave_addr,
                                                       FG6485A_REG_HUMIDITY,
                                                       2u, regs);
    if (s != MODBUS_OK) {
        return map_status(s);
    }

    out->humidity_pct   = (float)(int16_t)regs[0] / 10.0f;
    out->temperature_c  = (float)(int16_t)regs[1] / 10.0f;
    return FG6485A_OK;
}

fg6485a_status_t fg6485a_read_info(uint8_t slave_addr,
                                    fg6485a_info_t *out)
{
    if (slave_addr == 0u || out == nullptr) {
        return FG6485A_ERR_PARAM;
    }

    /* Read 4 registers starting at 0x0008:
     *   regs[0] = Device Type
     *   regs[1] = Version (low 8 byte)
     *   regs[2] = Device ID high 16 bit
     *   regs[3] = Device ID low  16 bit
     */
    uint16_t regs[4];
    modbus_status_t s = modbus_read_holding_registers(slave_addr,
                                                       FG6485A_REG_DEVICE_TYPE,
                                                       4u, regs);
    if (s != MODBUS_OK) {
        return map_status(s);
    }

    out->device_type = regs[0];
    out->version     = regs[1];
    out->device_id   = ((uint32_t)regs[2] << 16u) | (uint32_t)regs[3];
    return FG6485A_OK;
}

fg6485a_status_t fg6485a_read_all(uint8_t slave_addr,
                                   fg6485a_measurement_t *meas,
                                   fg6485a_info_t        *info)
{
    if (slave_addr == 0u) {
        return FG6485A_ERR_PARAM;
    }

    if (meas != nullptr) {
        fg6485a_status_t st = fg6485a_read_measurements(slave_addr, meas);
        if (st != FG6485A_OK) {
            return st;
        }
    }

    if (info != nullptr) {
        return fg6485a_read_info(slave_addr, info);
    }

    return FG6485A_OK;
}

fg6485a_status_t fg6485a_read_alarm_config(uint8_t slave_addr,
                                            fg6485a_alarm_config_t *out)
{
    if (slave_addr == 0u || out == nullptr) {
        return FG6485A_ERR_PARAM;
    }

    /* Read 8 registers starting at 0x000C:
     *   regs[0] = Temp upper alarm threshold × 10  (signed)
     *   regs[1] = Temp upper alarm enable   (0/1)
     *   regs[2] = Temp lower alarm threshold × 10  (signed)
     *   regs[3] = Temp lower alarm enable   (0/1)
     *   regs[4] = Hum  upper alarm threshold × 10
     *   regs[5] = Hum  upper alarm enable   (0/1)
     *   regs[6] = Hum  lower alarm threshold × 10
     *   regs[7] = Hum  lower alarm enable   (0/1)
     */
    uint16_t regs[8];
    modbus_status_t s = modbus_read_holding_registers(slave_addr,
                                                       FG6485A_REG_TEMP_ALARM_HI,
                                                       8u, regs);
    if (s != MODBUS_OK) {
        return map_status(s);
    }

    out->temp_alarm_high    = (float)(int16_t)regs[0] / 10.0f;
    out->temp_alarm_high_en = (regs[1] != 0u);
    out->temp_alarm_low     = (float)(int16_t)regs[2] / 10.0f;
    out->temp_alarm_low_en  = (regs[3] != 0u);
    out->hum_alarm_high     = (float)(int16_t)regs[4] / 10.0f;
    out->hum_alarm_high_en  = (regs[5] != 0u);
    out->hum_alarm_low      = (float)(int16_t)regs[6] / 10.0f;
    out->hum_alarm_low_en   = (regs[7] != 0u);
    return FG6485A_OK;
}

fg6485a_status_t fg6485a_write_alarm_config(uint8_t                       slave_addr,
                                             const fg6485a_alarm_config_t *cfg)
{
    if (slave_addr == 0u || cfg == nullptr) {
        return FG6485A_ERR_PARAM;
    }

    uint16_t regs[8];
    regs[0] = (uint16_t)(int16_t)(cfg->temp_alarm_high * 10.0f);
    regs[1] = cfg->temp_alarm_high_en ? 1u : 0u;
    regs[2] = (uint16_t)(int16_t)(cfg->temp_alarm_low  * 10.0f);
    regs[3] = cfg->temp_alarm_low_en  ? 1u : 0u;
    regs[4] = (uint16_t)(int16_t)(cfg->hum_alarm_high  * 10.0f);
    regs[5] = cfg->hum_alarm_high_en  ? 1u : 0u;
    regs[6] = (uint16_t)(int16_t)(cfg->hum_alarm_low   * 10.0f);
    regs[7] = cfg->hum_alarm_low_en   ? 1u : 0u;

    modbus_status_t s = modbus_write_multiple_registers(slave_addr,
                                                         FG6485A_REG_TEMP_ALARM_HI,
                                                         8u, regs);
    return map_status(s);
}

fg6485a_status_t fg6485a_write_temp_correction(uint8_t slave_addr,
                                                float   correction_c)
{
    if (slave_addr == 0u) {
        return FG6485A_ERR_PARAM;
    }

    uint16_t val = (uint16_t)(int16_t)(correction_c * 10.0f);
    modbus_status_t s = modbus_write_multiple_registers(slave_addr,
                                                         FG6485A_REG_TEMP_CORRECTION,
                                                         1u, &val);
    return map_status(s);
}

fg6485a_status_t fg6485a_write_humidity_correction(uint8_t slave_addr,
                                                    float   correction_pct)
{
    if (slave_addr == 0u) {
        return FG6485A_ERR_PARAM;
    }

    uint16_t val = (uint16_t)(int16_t)(correction_pct * 10.0f);
    modbus_status_t s = modbus_write_multiple_registers(slave_addr,
                                                         FG6485A_REG_HUM_CORRECTION,
                                                         1u, &val);
    return map_status(s);
}

/* ---------------------------------------------------------------------------
 * FreeRTOS polling task
 * --------------------------------------------------------------------------- */

#ifndef NATIVE_TEST
void fg6485a_task(void *pvParameters)
{
    fg6485a_task_param_t *param = static_cast<fg6485a_task_param_t *>(pvParameters);

    for (;;) {
        fg6485a_measurement_t local;
        fg6485a_status_t st = fg6485a_read_measurements(param->slave_addr, &local);

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
