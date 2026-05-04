/**
 * @file sensor_state.h
 * @brief Shared sensor state — Phase 3 / updated Phase 5.
 *
 * Single struct holding all register values for both emulated sensors.
 * Protected by a FreeRTOS mutex.  The modbus_slave_task reads from this
 * struct under the mutex; mode tasks (manual / live / replay) write to it
 * under the same mutex.
 *
 * Raw register encoding:
 *   FG6485A  — values stored × 10  (int16 for temperature, uint16 for humidity)
 *   S200     — values stored × 1000 (int32, big-endian word order on the bus)
 *
 * Default values match the NVS defaults defined in the design (§7):
 *   fg_temperature  = 250  (= 25.0 °C)
 *   fg_humidity     = 500  (= 50.0 %RH)
 *   S200 wind speed = 5000 (= 5.000 m/s)
 *   S200 wind dir   = 180000 (= 180.000 °)
 */

#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---------------------------------------------------------------------------
// Sensor operating mode (design §3)
// Phase 5: loaded from NVS; only MANUAL is active in Phases 1-9.
// LIVE is activated in Phase 10; REPLAY in Phase 11.
// ---------------------------------------------------------------------------

typedef enum {
    SENSOR_MODE_MANUAL = 0,
    SENSOR_MODE_LIVE   = 1,
    SENSOR_MODE_REPLAY = 2
} sensor_mode_t;

// ---------------------------------------------------------------------------
// FG6485A static device-info defaults
// These are read-only values returned for registers 0x0008–0x000B.
// ---------------------------------------------------------------------------
#define FG6485A_DEVICE_TYPE     0x0001u   // Arbitrary type code for T/RH sensor
#define FG6485A_VERSION         0x0100u   // Firmware v1.0
#define FG6485A_DEVICE_ID_HIGH  0x1234u   // Upper 16 bits of 32-bit device ID
#define FG6485A_DEVICE_ID_LOW   0x5678u   // Lower 16 bits of 32-bit device ID

// ---------------------------------------------------------------------------
// Shared state struct
// ---------------------------------------------------------------------------

typedef struct {

    // --- FG6485A ---

    // Operating mode (loaded from NVS; controls which source provides measurements)
    sensor_mode_t fg_mode;      // MANUAL / LIVE / REPLAY

    // Measurement (FC03 read, 0x0000–0x0001)
    uint16_t fg_humidity;       // reg 0x0000  e.g. 500 = 50.0 %RH
    int16_t  fg_temperature;    // reg 0x0001  e.g. 250 = 25.0 °C  (signed)

    // Device info (FC03 read, 0x0008–0x000B) — read-only, not writable
    uint16_t fg_device_type;    // reg 0x0008
    uint16_t fg_version;        // reg 0x0009
    uint16_t fg_device_id_high; // reg 0x000A
    uint16_t fg_device_id_low;  // reg 0x000B

    // Alarm config (FC03 read / FC16 write, 0x000C–0x0013)
    int16_t  fg_temp_alarm_hi;     // reg 0x000C
    uint16_t fg_temp_alarm_hi_en;  // reg 0x000D  0 or 1
    int16_t  fg_temp_alarm_lo;     // reg 0x000E
    uint16_t fg_temp_alarm_lo_en;  // reg 0x000F  0 or 1
    uint16_t fg_hum_alarm_hi;      // reg 0x0010
    uint16_t fg_hum_alarm_hi_en;   // reg 0x0011  0 or 1
    uint16_t fg_hum_alarm_lo;      // reg 0x0012
    uint16_t fg_hum_alarm_lo_en;   // reg 0x0013  0 or 1

    // Correction offsets (FC16 write-only, 0x001D–0x001E)
    // Stored so they can be read back if needed; not returned in FC03 reads.
    int16_t  fg_temp_correction;   // reg 0x001D
    int16_t  fg_hum_correction;    // reg 0x001E

    // --- S200 ---

    // Operating mode (loaded from NVS)
    sensor_mode_t s200_mode;    // MANUAL / LIVE / REPLAY

    // Wind / heating measurement (FC04 read, 0x0008–0x001D)
    int32_t s200_dir_min;    // regs 0x0008–0x0009
    int32_t s200_dir_max;    // regs 0x000A–0x000B
    int32_t s200_dir_avg;    // regs 0x000C–0x000D
    int32_t s200_spd_min;    // regs 0x000E–0x000F
    int32_t s200_spd_max;    // regs 0x0010–0x0011
    int32_t s200_spd_avg;    // regs 0x0012–0x0013
    int32_t s200_heat_high;  // regs 0x001C–0x001D  (heating temp high)
    int32_t s200_heat_low;   // regs 0x001E–0x001F  (heating temp low) — not in master frame but reserved

    // Config (FC03 read, 0x1000–0x1001)
    uint16_t s200_slave_addr_reg; // reg 0x1000  (mirrors the configured slave addr)
    uint16_t s200_baud_reg;       // reg 0x1001  9600 → 0x0001

    // --- Mutex ---
    SemaphoreHandle_t mutex;

} sensor_state_t;

// ---------------------------------------------------------------------------
// Global instance — defined in sensor_state.cpp, used by all modules
// ---------------------------------------------------------------------------
extern sensor_state_t g_sensor_state;

/**
 * Initialise g_sensor_state with default values and create the mutex.
 * Call once from setup() before starting any tasks.
 */
void sensor_state_init(void);
