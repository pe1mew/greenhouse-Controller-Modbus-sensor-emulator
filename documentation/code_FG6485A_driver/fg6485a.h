/**
 * @file fg6485a.h
 * @brief FG6485A Humidity and Temperature Transmitter driver — types and API.
 *
 * Thin driver over LIB-6 (modbus_rtu) for the ASAIR FG6485A RS485 Modbus
 * temperature/humidity sensor.  Provides:
 *   - Measurement reads  (FC03, registers 0x0000–0x0001)
 *   - Device info reads  (FC03, registers 0x0008–0x000B)
 *   - Alarm config read/write (FC03/FC16, registers 0x000C–0x0013)
 *   - Correction writes  (FC16, registers 0x001D–0x001E)
 *   - A FreeRTOS periodic polling task
 *
 * Prerequisites:
 *   Call modbus_init() (from modbus_rtu.h) once before using any function
 *   in this driver.
 *
 * Sensor specifications (from FG6485A datasheet v1.0):
 *   - Temperature range  : -40 to 120 °C,  accuracy ±0.3 °C, resolution 0.1 °C
 *   - Humidity range     : 0 to 99.9 %RH,  accuracy ±3 %RH,  resolution 0.1 %RH
 *   - Communication      : Modbus RTU, 9600 baud, 8N1, RS-485
 *   - Raw register values: integer × 10 of the actual engineering value
 *   - Slave address      : 1–255, set via 8-position DIP switch on PCB
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef NATIVE_TEST
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
#endif

/* ---------------------------------------------------------------------------
 * @defgroup fg6485a_addr Slave address default
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Factory-default Modbus slave address.
 *
 * The address is set by the 8-position DIP switch on the PCB.
 * Valid range is 1–255.  Address 0 (broadcast) is rejected by all read calls.
 */
#define FG6485A_DEFAULT_ADDR  1u

/** @} */

/* ---------------------------------------------------------------------------
 * @defgroup fg6485a_regs Register map (Modbus holding-register addresses)
 * @{
 * --------------------------------------------------------------------------- */

/* --- Measurement registers (read-only via FC03) --- */

/** @brief Relative humidity raw value × 10 (%RH).  Divide by 10 for actual value. */
#define FG6485A_REG_HUMIDITY          0x0000u

/** @brief Temperature raw value × 10 (°C, signed int16).  Divide by 10 for actual value. */
#define FG6485A_REG_TEMPERATURE       0x0001u

/* --- Device identification registers (read-only via FC03) --- */

/** @brief Device type identifier. */
#define FG6485A_REG_DEVICE_TYPE       0x0008u

/** @brief Firmware version (low 8 bytes). */
#define FG6485A_REG_VERSION           0x0009u

/** @brief Device ID, high 16 bits. */
#define FG6485A_REG_DEVICE_ID_HIGH    0x000Au

/** @brief Device ID, low 16 bits. */
#define FG6485A_REG_DEVICE_ID_LOW     0x000Bu

/* --- Alarm configuration registers (read via FC03, write via FC16) ---
 * Writable range: 0x000C–0x001E.  Values outside valid range are clamped
 * by the device; enable registers accept 0 (off) or 1 (on).
 * The host must send threshold values magnified × 10 (integer). */

/** @brief Temperature upper alarm threshold raw × 10 (°C, signed). */
#define FG6485A_REG_TEMP_ALARM_HI     0x000Cu

/** @brief Temperature upper alarm enable (1 = on, 0 = off). */
#define FG6485A_REG_TEMP_ALARM_HI_EN  0x000Du

/** @brief Temperature lower alarm threshold raw × 10 (°C, signed). */
#define FG6485A_REG_TEMP_ALARM_LO     0x000Eu

/** @brief Temperature lower alarm enable (1 = on, 0 = off). */
#define FG6485A_REG_TEMP_ALARM_LO_EN  0x000Fu

/** @brief Humidity upper alarm threshold raw × 10 (%RH). */
#define FG6485A_REG_HUM_ALARM_HI      0x0010u

/** @brief Humidity upper alarm enable (1 = on, 0 = off). */
#define FG6485A_REG_HUM_ALARM_HI_EN   0x0011u

/** @brief Humidity lower alarm threshold raw × 10 (%RH). */
#define FG6485A_REG_HUM_ALARM_LO      0x0012u

/** @brief Humidity lower alarm enable (1 = on, 0 = off). */
#define FG6485A_REG_HUM_ALARM_LO_EN   0x0013u

/* --- Correction registers (write-only via FC16) --- */

/** @brief Temperature correction offset raw × 10 (°C, signed). */
#define FG6485A_REG_TEMP_CORRECTION   0x001Du

/** @brief Humidity correction offset raw × 10 (%RH, signed). */
#define FG6485A_REG_HUM_CORRECTION    0x001Eu

/** @} */ /* end fg6485a_regs */

/* ---------------------------------------------------------------------------
 * @defgroup fg6485a_types Data types
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Return codes for all FG6485A API functions.
 */
typedef enum {
    FG6485A_OK        = 0, /**< Operation completed successfully. */
    FG6485A_ERR_COMM  = 1, /**< Modbus communication error (timeout / CRC / exception). */
    FG6485A_ERR_PARAM = 2, /**< Caller supplied an invalid parameter (NULL pointer, addr=0). */
} fg6485a_status_t;

/**
 * @brief Measured temperature and relative humidity.
 */
typedef struct {
    float temperature_c;  /**< Temperature in °C (resolution 0.1 °C, range -40…120). */
    float humidity_pct;   /**< Relative humidity in %RH (resolution 0.1 %RH, range 0…99.9). */
} fg6485a_measurement_t;

/**
 * @brief Device identification information.
 */
typedef struct {
    uint16_t device_type; /**< Device type code from register 0x0008. */
    uint16_t version;     /**< Firmware version from register 0x0009. */
    uint32_t device_id;   /**< 32-bit ID: (reg_0x000A << 16) | reg_0x000B. */
} fg6485a_info_t;

/**
 * @brief Alarm threshold and enable configuration.
 *
 * Threshold values are in engineering units (°C or %RH).
 * The driver multiplies by 10 internally when writing to the device.
 */
typedef struct {
    float temp_alarm_high;    /**< Temperature upper alarm threshold (°C). */
    bool  temp_alarm_high_en; /**< True to enable temperature upper alarm. */
    float temp_alarm_low;     /**< Temperature lower alarm threshold (°C). */
    bool  temp_alarm_low_en;  /**< True to enable temperature lower alarm. */
    float hum_alarm_high;     /**< Humidity upper alarm threshold (%RH). */
    bool  hum_alarm_high_en;  /**< True to enable humidity upper alarm. */
    float hum_alarm_low;      /**< Humidity lower alarm threshold (%RH). */
    bool  hum_alarm_low_en;   /**< True to enable humidity lower alarm. */
} fg6485a_alarm_config_t;

#ifndef NATIVE_TEST
/**
 * @brief Parameters for the FreeRTOS periodic polling task.
 *
 * Allocate statically or on the heap and pass as @c pvParameters when
 * creating the task via @c xTaskCreate() or @c xTaskCreatePinnedToCore().
 * All fields must be initialised before the task starts; @p mutex must be
 * created with @c xSemaphoreCreateMutex() by the caller.
 *
 * @par Typical usage
 * @code
 *   static fg6485a_measurement_t g_meas;
 *   static fg6485a_status_t      g_status;
 *   static fg6485a_task_param_t  g_param = {
 *       .slave_addr       = FG6485A_DEFAULT_ADDR,
 *       .poll_interval_ms = 5000,
 *       .out_data         = &g_meas,
 *       .out_status       = &g_status,
 *       .mutex            = NULL,   // created below
 *   };
 *
 *   void app_main(void) {
 *       modbus_init();
 *       g_param.mutex = xSemaphoreCreateMutex();
 *       xTaskCreatePinnedToCore(fg6485a_task, "fg6485a", 2048,
 *                               &g_param, 5, NULL, APP_CPU_NUM);
 *   }
 * @endcode
 */
typedef struct {
    uint8_t                slave_addr;        /**< Modbus slave address (1–255). */
    uint32_t               poll_interval_ms;  /**< Measurement interval in milliseconds. */
    fg6485a_measurement_t *out_data;          /**< Shared output buffer (protected by mutex). */
    fg6485a_status_t      *out_status;        /**< Last operation status (protected by mutex). */
    SemaphoreHandle_t      mutex;             /**< Caller-created mutex protecting the two pointers above. */
} fg6485a_task_param_t;
#endif /* NATIVE_TEST */

/** @} */ /* end fg6485a_types */

/* ---------------------------------------------------------------------------
 * @defgroup fg6485a_api API
 * @{
 * --------------------------------------------------------------------------- */

/**
 * @brief Read the current temperature and humidity measurements.
 *
 * Sends one FC03 request for registers @ref FG6485A_REG_HUMIDITY and
 * @ref FG6485A_REG_TEMPERATURE (0x0000–0x0001).  Raw ×10 integer values are
 * divided by 10 before storing in @p out.
 *
 * @param slave_addr  Modbus slave address of the sensor (1–255).
 * @param out         Caller-supplied @ref fg6485a_measurement_t to fill.
 * @return @ref FG6485A_OK, @ref FG6485A_ERR_COMM, or @ref FG6485A_ERR_PARAM.
 */
fg6485a_status_t fg6485a_read_measurements(uint8_t slave_addr,
                                            fg6485a_measurement_t *out);

/**
 * @brief Read device type, firmware version, and 32-bit device ID.
 *
 * Sends one FC03 request for registers 0x0008–0x000B.
 *
 * @param slave_addr  Modbus slave address of the sensor.
 * @param out         Caller-supplied @ref fg6485a_info_t to fill.
 * @return @ref FG6485A_OK, @ref FG6485A_ERR_COMM, or @ref FG6485A_ERR_PARAM.
 */
fg6485a_status_t fg6485a_read_info(uint8_t slave_addr,
                                    fg6485a_info_t *out);

/**
 * @brief Read measurements and device info in a single call.
 *
 * Issues up to two FC03 requests sequentially.  Either @p meas or @p info
 * may be NULL to skip that read.  Returns the first error encountered.
 *
 * @param slave_addr  Modbus slave address of the sensor.
 * @param meas        Output for measurement data, or NULL to skip.
 * @param info        Output for device info, or NULL to skip.
 * @return @ref FG6485A_OK if all requested reads succeeded; first error code otherwise.
 */
fg6485a_status_t fg6485a_read_all(uint8_t slave_addr,
                                   fg6485a_measurement_t *meas,
                                   fg6485a_info_t        *info);

/**
 * @brief Read all alarm threshold and enable registers (0x000C–0x0013).
 *
 * Sends one FC03 request for 8 registers.  Raw ×10 threshold values are
 * converted to float before returning.
 *
 * @param slave_addr  Modbus slave address of the sensor.
 * @param out         Caller-supplied @ref fg6485a_alarm_config_t to fill.
 * @return @ref FG6485A_OK, @ref FG6485A_ERR_COMM, or @ref FG6485A_ERR_PARAM.
 */
fg6485a_status_t fg6485a_read_alarm_config(uint8_t slave_addr,
                                            fg6485a_alarm_config_t *out);

/**
 * @brief Write alarm threshold and enable registers (0x000C–0x0013).
 *
 * Sends one FC16 request for 8 registers.  Float threshold values are
 * multiplied by 10 and cast to int16_t before writing.
 *
 * @param slave_addr  Modbus slave address of the sensor.
 * @param cfg         Alarm configuration to write (must not be NULL).
 * @return @ref FG6485A_OK, @ref FG6485A_ERR_COMM, or @ref FG6485A_ERR_PARAM.
 */
fg6485a_status_t fg6485a_write_alarm_config(uint8_t                       slave_addr,
                                             const fg6485a_alarm_config_t *cfg);

/**
 * @brief Write a temperature correction offset to register 0x001D.
 *
 * @param slave_addr    Modbus slave address of the sensor.
 * @param correction_c  Offset in °C (multiplied by 10 and written as int16).
 * @return @ref FG6485A_OK, @ref FG6485A_ERR_COMM, or @ref FG6485A_ERR_PARAM.
 */
fg6485a_status_t fg6485a_write_temp_correction(uint8_t slave_addr,
                                                float   correction_c);

/**
 * @brief Write a humidity correction offset to register 0x001E.
 *
 * @param slave_addr      Modbus slave address of the sensor.
 * @param correction_pct  Offset in %RH (multiplied by 10 and written as int16).
 * @return @ref FG6485A_OK, @ref FG6485A_ERR_COMM, or @ref FG6485A_ERR_PARAM.
 */
fg6485a_status_t fg6485a_write_humidity_correction(uint8_t slave_addr,
                                                    float   correction_pct);

#ifndef NATIVE_TEST
/**
 * @brief FreeRTOS periodic task — polls temperature and humidity on a fixed interval.
 *
 * Pass as the task function to @c xTaskCreate() or @c xTaskCreatePinnedToCore().
 * @p pvParameters must point to a fully initialised @ref fg6485a_task_param_t.
 * The task never returns; call @c vTaskDelete(handle) to stop it.
 *
 * @par Recommended task configuration
 *   - Stack : 2048 words
 *   - Priority: 5 (sensor poll; below time-critical, above idle)
 *   - Core  : APP_CPU_NUM (core 1) to keep PRO_CPU free for comms
 *
 * @param pvParameters  Pointer to @ref fg6485a_task_param_t (must not be NULL).
 */
void fg6485a_task(void *pvParameters);
#endif /* NATIVE_TEST */

/** @} */ /* end fg6485a_api */
