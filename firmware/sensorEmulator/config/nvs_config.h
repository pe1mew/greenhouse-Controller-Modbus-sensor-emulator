/**
 * @file nvs_config.h
 * @brief Persistent settings via ESP-IDF NVS — Phase 5.
 *
 * All configurable values (design §7) are stored under the NVS namespace
 * "emulator" and survive a power cycle.  On first boot (or after an NVS
 * erase) every getter returns the supplied default.
 *
 * NVS key names — all ≤ 15 characters (ESP-IDF NVS_KEY_NAME_MAX_SIZE = 16):
 *
 * Key               Type    Default          Description
 * ─────────────────────────────────────────────────────────────────────────
 * fg_slave_addr     u8      1                FG6485A Modbus slave address
 * fg_mode           u8      0 (MANUAL)       FG6485A operating mode
 * fg_temp_manual    i16     250  (= 25.0 °C) FG6485A manual temperature ×10
 * fg_hum_manual     u16     500  (= 50.0 %)  FG6485A manual humidity ×10
 * s200_slave_addr   u8      44               S200 Modbus slave address
 * s200_mode         u8      0 (MANUAL)       S200 operating mode
 * s200_spd_manual   i32     5000 (= 5.000)   S200 manual wind speed ×1000
 * s200_dir_manual   i32     180000 (=180.000)S200 manual wind direction ×1000
 * live_lat          float   52.37            Live mode latitude  (Phase 10)
 * live_lon          float   4.90             Live mode longitude (Phase 10)
 * replay_file       str     ""               LittleFS replay CSV path (Phase 11)
 * wifi_ssid         str     ""               STA network SSID   (Phase 6)
 * wifi_pass         str     ""               STA network password
 * ntp_server        str     "pool.ntp.org"   NTP server hostname (Phase 9)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// NVS namespace (max 15 chars per ESP-IDF NVS_KEY_NAME_MAX_SIZE constraint)
// ---------------------------------------------------------------------------

/** @brief ESP-IDF NVS namespace used for all emulator settings. */
#define NVS_NAMESPACE  "emulator"

// ---------------------------------------------------------------------------
// NVS key constants
// All strings are \u2264 15 characters (ESP-IDF hard limit is 16 including NUL).
// ---------------------------------------------------------------------------

/** @brief NVS key — FG6485A Modbus slave address (@c uint8_t, default 1). */
#define NVS_KEY_FG_SLAVE_ADDR   "fg_slave_addr"

/** @brief NVS key — FG6485A operating mode (@c uint8_t = @c sensor_mode_t, default MANUAL). */
#define NVS_KEY_FG_MODE         "fg_mode"

/** @brief NVS key — FG6485A manual temperature raw value (@c int16_t ×10, default 250 = 25.0 °C). */
#define NVS_KEY_FG_TEMP         "fg_temp_manual"

/** @brief NVS key — FG6485A manual humidity raw value (@c uint16_t ×10, default 500 = 50.0 %RH). */
#define NVS_KEY_FG_HUM          "fg_hum_manual"

/** @brief NVS key — S200 Modbus slave address (@c uint8_t, default 44). */
#define NVS_KEY_S200_SLAVE_ADDR "s200_slave_addr"

/** @brief NVS key — S200 operating mode (@c uint8_t = @c sensor_mode_t, default MANUAL). */
#define NVS_KEY_S200_MODE       "s200_mode"

/** @brief NVS key — S200 manual wind speed raw value (@c int32_t ×1000, default 5000 = 5.000 m/s). */
#define NVS_KEY_S200_SPD        "s200_spd_manual"

/** @brief NVS key — S200 manual wind direction raw value (@c int32_t ×1000, default 180000 = 180.000°). */
#define NVS_KEY_S200_DIR        "s200_dir_manual"

/** @brief NVS key — Live mode latitude (@c float blob 4 B, default 52.37). */
#define NVS_KEY_LIVE_LAT        "live_lat"

/** @brief NVS key — Live mode longitude (@c float blob 4 B, default 4.90). */
#define NVS_KEY_LIVE_LON        "live_lon"

/** @brief NVS key — LittleFS path to the replay CSV file (@c str, default ""). */
#define NVS_KEY_REPLAY_FILE     "replay_file"

/** @brief NVS key — WiFi STA SSID (@c str, default ""). */
#define NVS_KEY_WIFI_SSID       "wifi_ssid"

/** @brief NVS key — WiFi STA password (@c str, default ""). */
#define NVS_KEY_WIFI_PASS       "wifi_pass"

/** @brief NVS key — NTP server hostname (@c str, default "pool.ntp.org"). */
#define NVS_KEY_NTP_SERVER      "ntp_server"

// ---------------------------------------------------------------------------
// String field size limits (bytes, including null terminator)
// ---------------------------------------------------------------------------

/** @brief Maximum length of the replay CSV file path string (incl. NUL). */
#define NVS_STR_MAX_PATH    64

/** @brief Maximum length of the WiFi SSID string (incl. NUL). */
#define NVS_STR_MAX_SSID    33

/** @brief Maximum length of the WiFi password string (incl. NUL). */
#define NVS_STR_MAX_PASS    64

/** @brief Maximum length of the NTP server hostname string (incl. NUL). */
#define NVS_STR_MAX_NTP     64

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Initialise NVS flash storage and open the @c "emulator" namespace.
 *
 * Calls @c nvs_flash_init().  If the partition is corrupt
 * (@c ESP_ERR_NVS_NO_FREE_PAGES) or was written by a different NVS version
 * (@c ESP_ERR_NVS_NEW_VERSION_FOUND), the partition is erased and
 * re-initialised.  Opens the @c NVS_NAMESPACE handle which is kept open for
 * the firmware lifetime.
 *
 * Must be called once from @c setup() before any other @c nvs_cfg_* function.
 */
void nvs_cfg_init(void);

/**
 * @brief Read a @c uint8_t value from NVS.
 * @param key  NVS key string (must match a @c NVS_KEY_* constant).
 * @param def  Value returned when the key is absent or an error occurs.
 * @return     Stored value, or @p def on miss/error.
 */
uint8_t  nvs_cfg_get_u8   (const char *key, uint8_t   def);

/**
 * @brief Read a @c int16_t value from NVS.
 * @param key  NVS key string.
 * @param def  Default value.
 * @return     Stored value, or @p def on miss/error.
 */
int16_t  nvs_cfg_get_i16  (const char *key, int16_t   def);

/**
 * @brief Read a @c uint16_t value from NVS.
 * @param key  NVS key string.
 * @param def  Default value.
 * @return     Stored value, or @p def on miss/error.
 */
uint16_t nvs_cfg_get_u16  (const char *key, uint16_t  def);

/**
 * @brief Read a @c int32_t value from NVS.
 * @param key  NVS key string.
 * @param def  Default value.
 * @return     Stored value, or @p def on miss/error.
 */
int32_t  nvs_cfg_get_i32  (const char *key, int32_t   def);

/**
 * @brief Read a @c float value from NVS (stored as a 4-byte blob).
 * @param key  NVS key string.
 * @param def  Default value.
 * @return     Stored value, or @p def on miss/error.
 */
float    nvs_cfg_get_float(const char *key, float     def);

/**
 * @brief Read a null-terminated string from NVS.
 *
 * If the key is absent or an error occurs, @p out is set to @p def.
 * @p out is always null-terminated.
 *
 * @param key      NVS key string.
 * @param out      Destination buffer.
 * @param max_len  Size of @p out in bytes (including the null terminator).
 * @param def      Default string (may be @c "").
 */
void     nvs_cfg_get_str  (const char *key, char *out, size_t max_len,
                            const char *def);

/**
 * @brief Write a @c uint8_t value to NVS and commit immediately.
 * @param key  NVS key string.
 * @param val  Value to store.
 */
void nvs_cfg_set_u8   (const char *key, uint8_t   val);

/**
 * @brief Write a @c int16_t value to NVS and commit immediately.
 * @param key  NVS key string.
 * @param val  Value to store.
 */
void nvs_cfg_set_i16  (const char *key, int16_t   val);

/**
 * @brief Write a @c uint16_t value to NVS and commit immediately.
 * @param key  NVS key string.
 * @param val  Value to store.
 */
void nvs_cfg_set_u16  (const char *key, uint16_t  val);

/**
 * @brief Write a @c int32_t value to NVS and commit immediately.
 * @param key  NVS key string.
 * @param val  Value to store.
 */
void nvs_cfg_set_i32  (const char *key, int32_t   val);

/**
 * @brief Write a @c float value to NVS as a 4-byte blob and commit immediately.
 * @param key  NVS key string.
 * @param val  Value to store.
 */
void nvs_cfg_set_float(const char *key, float     val);

/**
 * @brief Write a null-terminated string to NVS and commit immediately.
 * @param key  NVS key string.
 * @param val  Null-terminated string to store.
 */
void nvs_cfg_set_str  (const char *key, const char *val);

/**
 * @brief Load all NVS settings into @c g_sensor_state and return slave addresses.
 *
 * Reads every NVS key and:
 *   - Populates the manual sensor values and operating modes in @c g_sensor_state.
 *   - Returns the configured Modbus slave addresses via the output parameters.
 *
 * Call after nvs_cfg_init() and sensor_state_init(), before creating any
 * FreeRTOS tasks.  The sensor state mutex is **not** taken — values are
 * written directly because no tasks are running yet.
 *
 * @param[out] fg_addr_out    FG6485A Modbus slave address from NVS (default 1).
 * @param[out] s200_addr_out  S200 Modbus slave address from NVS (default 44).
 */
void nvs_cfg_load_all(uint8_t *fg_addr_out, uint8_t *s200_addr_out);
