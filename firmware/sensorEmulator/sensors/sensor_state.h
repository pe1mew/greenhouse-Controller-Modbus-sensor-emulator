/**
 * @file sensor_state.h
 * @brief Shared sensor state struct and synchronisation primitives — Phase 3.
 *
 * Defines a single @c sensor_state_t struct that holds every register value
 * for both emulated sensors (FG6485A and SenseCAP S200).  All tasks that
 * read or write sensor data must acquire @c g_sensor_state.mutex before
 * accessing the struct and release it immediately after.
 *
 * Raw register encoding:
 *   - FG6485A: values stored × 10 (@c int16_t for temperature, @c uint16_t
 *     for humidity).  Example: 250 → 25.0 °C.
 *   - S200: values stored × 1000 (@c int32_t, transmitted as two consecutive
 *     big-endian 16-bit registers on the bus).  Example: 180000 → 180.000°.
 *
 * Default values (applied by sensor_state_init() and overridden by NVS on
 * subsequent boots):
 *   | Field              | Default  | Physical value     |
 *   |:-------------------|:---------|:-------------------|
 *   | fg_temperature     |  250     | 25.0 °C            |
 *   | fg_humidity        |  500     | 50.0 %RH           |
 *   | s200_spd_min/max/avg| 5000    | 5.000 m/s          |
 *   | s200_dir_min/max/avg| 180000  | 180.000°           |
 *   | s200_heat_high     | 25000    | 25.000 °C          |
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

/**
 * @brief Sensor operating mode — controls which data source provides
 *        measurement values for a given sensor.
 *
 * Only SENSOR_MODE_MANUAL is active in Phases 1–9.
 * SENSOR_MODE_LIVE is activated in Phase 10 (Open-Meteo API fetch).
 * SENSOR_MODE_REPLAY is activated in Phase 11 (CSV playback).
 *
 * The active mode is loaded from NVS at boot (Phase 5) and updated via
 * the web interface POST handler (Phase 8).
 */
typedef enum {
    /** Values come from the manually-configured NVS registers. */
    SENSOR_MODE_MANUAL = 0,
    /** Values fetched periodically from the Open-Meteo weather API. */
    SENSOR_MODE_LIVE   = 1,
    /** Values replayed from a timestamped CSV file on LittleFS. */
    SENSOR_MODE_REPLAY = 2,
    /** Values injected by an external source via POST /api/data. */
    SENSOR_MODE_REST   = 3
} sensor_mode_t;

// ---------------------------------------------------------------------------
// FG6485A static device-info defaults
// These are read-only values returned for registers 0x0008–0x000B.
// ---------------------------------------------------------------------------

/** @brief Device type code returned in FG6485A register 0x0008. */
#define FG6485A_DEVICE_TYPE     0x0001u

/** @brief Firmware version returned in FG6485A register 0x0009 (v1.0). */
#define FG6485A_VERSION         0x0100u

/** @brief Upper 16 bits of the 32-bit device ID (registers 0x000A). */
#define FG6485A_DEVICE_ID_HIGH  0x1234u

/** @brief Lower 16 bits of the 32-bit device ID (register 0x000B). */
#define FG6485A_DEVICE_ID_LOW   0x5678u

// ---------------------------------------------------------------------------
// Physical range constants (design §11.1)
// Used for input clamping in web POST handlers (Phase 8), replay task
// (Phase 11), and live-fetch value injection (Phase 10).
// Raw encoding mirrors the register encoding described in the file header.
// ---------------------------------------------------------------------------

/** @defgroup fg_range FG6485A physical range (raw register units)
 *  Temperature: raw = physical_°C × 10  (int16_t)
 *  Humidity:    raw = physical_%RH × 10 (uint16_t)
 * @{
 */
/** @brief FG6485A minimum temperature raw value: −40 °C → −400. */
#define FG6485A_TEMP_RAW_MIN   ((int16_t)(-400))
/** @brief FG6485A maximum temperature raw value: 120 °C → 1200. */
#define FG6485A_TEMP_RAW_MAX   ((int16_t)(1200))
/** @brief FG6485A minimum humidity raw value: 0.0 %RH → 0. */
#define FG6485A_HUM_RAW_MIN    ((uint16_t)(0))
/** @brief FG6485A maximum humidity raw value: 99.9 %RH → 999. */
#define FG6485A_HUM_RAW_MAX    ((uint16_t)(999))
/** @} */

/** @defgroup s200_range SenseCAP S200 physical range (raw register units)
 *  All S200 values: raw = physical × 1000  (int32_t)
 * @{
 */
/** @brief S200 minimum wind speed raw value: 0 m/s → 0. */
#define S200_SPD_RAW_MIN       ((int32_t)(0))
/** @brief S200 maximum wind speed raw value: 60 m/s → 60 000. */
#define S200_SPD_RAW_MAX       ((int32_t)(60000))
/** @brief S200 minimum wind direction raw value: 0° → 0. */
#define S200_DIR_RAW_MIN       ((int32_t)(0))
/** @brief S200 maximum wind direction raw value: 360° → 360 000. */
#define S200_DIR_RAW_MAX       ((int32_t)(360000))
/** @brief S200 minimum heating temperature raw value: −40 °C → −40 000. */
#define S200_HEAT_RAW_MIN      ((int32_t)(-40000))
/** @brief S200 maximum heating temperature raw value: 85 °C → 85 000. */
#define S200_HEAT_RAW_MAX      ((int32_t)(85000))
/** @} */

// ---------------------------------------------------------------------------
// Shared state struct
// ---------------------------------------------------------------------------

/**
 * @brief Aggregated emulator state for all sensors.
 *
 * Single instance @ref g_sensor_state.  All fields are protected by the
 * embedded @c mutex; callers must hold the mutex for the duration of any
 * read or write operation.
 */
typedef struct {

    // --- FG6485A ---

    /** @brief FG6485A operating mode (MANUAL / LIVE / REPLAY); loaded from NVS. */
    sensor_mode_t fg_mode;

    /**
     * @brief FG6485A relative humidity raw value (FC03 register 0x0000).
     * Encoding: physical %RH × 10.  Example: 500 → 50.0 %RH.
     */
    uint16_t fg_humidity;

    /**
     * @brief FG6485A temperature raw value (FC03 register 0x0001).
     * Encoding: physical °C × 10, signed.  Example: 250 → 25.0 °C.
     */
    int16_t  fg_temperature;

    /**
     * @name FG6485A device info (FC03 registers 0x0008–0x000B)
     * Read-only fields initialised from compile-time constants.
     * @{
     */
    uint16_t fg_device_type;    /**< reg 0x0008 — device type code. */
    uint16_t fg_version;        /**< reg 0x0009 — firmware version. */
    uint16_t fg_device_id_high; /**< reg 0x000A — upper 16 bits of device ID. */
    uint16_t fg_device_id_low;  /**< reg 0x000B — lower 16 bits of device ID. */
    /** @} */

    /**
     * @name FG6485A alarm configuration (FC03 read / FC16 write, 0x000C–0x0013)
     * @{
     */
    int16_t  fg_temp_alarm_hi;     /**< reg 0x000C — high temperature alarm threshold (×10). */
    uint16_t fg_temp_alarm_hi_en;  /**< reg 0x000D — high temperature alarm enable (0 = off, 1 = on). */
    int16_t  fg_temp_alarm_lo;     /**< reg 0x000E — low temperature alarm threshold (×10). */
    uint16_t fg_temp_alarm_lo_en;  /**< reg 0x000F — low temperature alarm enable (0 = off, 1 = on). */
    uint16_t fg_hum_alarm_hi;      /**< reg 0x0010 — high humidity alarm threshold (×10). */
    uint16_t fg_hum_alarm_hi_en;   /**< reg 0x0011 — high humidity alarm enable (0 = off, 1 = on). */
    uint16_t fg_hum_alarm_lo;      /**< reg 0x0012 — low humidity alarm threshold (×10). */
    uint16_t fg_hum_alarm_lo_en;   /**< reg 0x0013 — low humidity alarm enable (0 = off, 1 = on). */
    /** @} */

    /**
     * @name FG6485A correction offsets (FC16 write-only, 0x001D–0x001E)
     * Stored so they can be read back internally; not returned by FC03 reads.
     * @{
     */
    int16_t  fg_temp_correction;   /**< reg 0x001D — temperature correction offset (×10). */
    int16_t  fg_hum_correction;    /**< reg 0x001E — humidity correction offset (×10). */
    /** @} */

    // --- S200 ---

    /** @brief S200 operating mode (MANUAL / LIVE / REPLAY); loaded from NVS. */
    sensor_mode_t s200_mode;

    /**
     * @name S200 wind measurement (FC04 registers 0x0008–0x0013)
     * All values are @c int32_t × 1000.  Each value occupies two consecutive
     * big-endian 16-bit registers on the bus (high word first).
     * @{
     */
    int32_t s200_dir_min;  /**< regs 0x0008–0x0009 — wind direction minimum (×1000 deg). */
    int32_t s200_dir_max;  /**< regs 0x000A–0x000B — wind direction maximum (×1000 deg). */
    int32_t s200_dir_avg;  /**< regs 0x000C–0x000D — wind direction average (×1000 deg). */
    int32_t s200_spd_min;  /**< regs 0x000E–0x000F — wind speed minimum (×1000 m/s). */
    int32_t s200_spd_max;  /**< regs 0x0010–0x0011 — wind speed maximum (×1000 m/s). */
    int32_t s200_spd_avg;  /**< regs 0x0012–0x0013 — wind speed average (×1000 m/s). */
    /** @} */

    /**
     * @name S200 heating temperature (FC04 registers 0x001C–0x001F)
     * @{
     */
    int32_t s200_heat_high; /**< regs 0x001C–0x001D — heating temperature high threshold (×1000 °C). */
    int32_t s200_heat_low;  /**< regs 0x001E–0x001F — heating temperature low threshold (×1000 °C). */
    /** @} */

    /**
     * @name S200 configuration (FC03 registers 0x1000–0x1001)
     * @{
     */
    uint16_t s200_slave_addr_reg; /**< reg 0x1000 — mirrors the configured Modbus slave address. */
    uint16_t s200_baud_reg;       /**< reg 0x1001 — baud rate code (1 = 9600 baud). */
    /** @} */

    /**
     * @brief FreeRTOS mutex protecting the entire struct.
     *
     * All tasks must call xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY)
     * before reading or writing any field, and xSemaphoreGive() immediately
     * after.  Created by sensor_state_init().
     */
    SemaphoreHandle_t mutex;

} sensor_state_t;

/**
 * @brief Singleton instance of the shared sensor state.
 *
 * Defined in sensor_state.cpp.  All modules access sensor data through this
 * global.  Always acquire @c g_sensor_state.mutex before reading or writing.
 */
extern sensor_state_t g_sensor_state;

/**
 * @brief Initialise @c g_sensor_state with design-default values and create
 *        the FreeRTOS mutex.
 *
 * Sets every field to its NVS default (as defined in the design, §7) and
 * calls xSemaphoreCreateMutex().  Must be called once from @c setup() before
 * any task is started or any @c nvs_cfg_* getter is called.
 */
void sensor_state_init(void);
