/**
 * @file sensor_state.cpp
 * @brief Shared sensor state — initialisation with default values.
 */

#include "sensor_state.h"

sensor_state_t g_sensor_state;

void sensor_state_init(void)
{
    // FG6485A mode (overridden by NVS in Phase 5)
    g_sensor_state.fg_mode = SENSOR_MODE_MANUAL;

    // FG6485A measurement defaults
    g_sensor_state.fg_humidity    = 500;   // 50.0 %RH
    g_sensor_state.fg_temperature = 250;   // 25.0 °C

    // FG6485A static device info
    g_sensor_state.fg_device_type    = FG6485A_DEVICE_TYPE;
    g_sensor_state.fg_version        = FG6485A_VERSION;
    g_sensor_state.fg_device_id_high = FG6485A_DEVICE_ID_HIGH;
    g_sensor_state.fg_device_id_low  = FG6485A_DEVICE_ID_LOW;

    // FG6485A alarm config defaults (disabled, sensible thresholds)
    g_sensor_state.fg_temp_alarm_hi    = 400;  // 40.0 °C
    g_sensor_state.fg_temp_alarm_hi_en = 0;
    g_sensor_state.fg_temp_alarm_lo    = 0;    // 0.0 °C
    g_sensor_state.fg_temp_alarm_lo_en = 0;
    g_sensor_state.fg_hum_alarm_hi     = 900;  // 90.0 %RH
    g_sensor_state.fg_hum_alarm_hi_en  = 0;
    g_sensor_state.fg_hum_alarm_lo     = 100;  // 10.0 %RH
    g_sensor_state.fg_hum_alarm_lo_en  = 0;

    // FG6485A correction offsets (zero by default)
    g_sensor_state.fg_temp_correction = 0;
    g_sensor_state.fg_hum_correction  = 0;

    // S200 mode (overridden by NVS in Phase 5)
    g_sensor_state.s200_mode = SENSOR_MODE_MANUAL;

    // S200 measurement defaults
    g_sensor_state.s200_dir_min   = 180000;  // 180.000 °
    g_sensor_state.s200_dir_max   = 180000;
    g_sensor_state.s200_dir_avg   = 180000;
    g_sensor_state.s200_spd_min   = 5000;    // 5.000 m/s
    g_sensor_state.s200_spd_max   = 5000;
    g_sensor_state.s200_spd_avg   = 5000;
    g_sensor_state.s200_heat_high = 25000;   // 25.000 °C
    g_sensor_state.s200_heat_low  = 25000;

    // S200 config registers
    g_sensor_state.s200_slave_addr_reg = 44;
    g_sensor_state.s200_baud_reg       = 1;  // 9600 baud code

    // Create the shared mutex
    g_sensor_state.mutex = xSemaphoreCreateMutex();
}
