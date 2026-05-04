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
// NVS namespace (max 15 chars)
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE  "emulator"

// ---------------------------------------------------------------------------
// NVS key constants (all ≤ 15 chars)
// ---------------------------------------------------------------------------
#define NVS_KEY_FG_SLAVE_ADDR   "fg_slave_addr"    // u8
#define NVS_KEY_FG_MODE         "fg_mode"           // u8
#define NVS_KEY_FG_TEMP         "fg_temp_manual"    // i16
#define NVS_KEY_FG_HUM          "fg_hum_manual"     // u16
#define NVS_KEY_S200_SLAVE_ADDR "s200_slave_addr"   // u8
#define NVS_KEY_S200_MODE       "s200_mode"         // u8
#define NVS_KEY_S200_SPD        "s200_spd_manual"   // i32
#define NVS_KEY_S200_DIR        "s200_dir_manual"   // i32
#define NVS_KEY_LIVE_LAT        "live_lat"          // float (blob 4 B)
#define NVS_KEY_LIVE_LON        "live_lon"          // float (blob 4 B)
#define NVS_KEY_REPLAY_FILE     "replay_file"       // str
#define NVS_KEY_WIFI_SSID       "wifi_ssid"         // str
#define NVS_KEY_WIFI_PASS       "wifi_pass"         // str
#define NVS_KEY_NTP_SERVER      "ntp_server"        // str

// ---------------------------------------------------------------------------
// String field size limits (including null terminator)
// ---------------------------------------------------------------------------
#define NVS_STR_MAX_PATH    64   // replay_file
#define NVS_STR_MAX_SSID    33   // wifi_ssid
#define NVS_STR_MAX_PASS    64   // wifi_pass
#define NVS_STR_MAX_NTP     64   // ntp_server

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * Initialise NVS flash and open the "emulator" namespace.
 * If the partition is corrupt or version-changed it is erased and
 * re-initialised.  Must be called once from setup() before any other
 * nvs_cfg_* function.
 */
void nvs_cfg_init(void);

// --- Typed getters (return def if the key is absent or an error occurs) ---
uint8_t  nvs_cfg_get_u8   (const char *key, uint8_t   def);
int16_t  nvs_cfg_get_i16  (const char *key, int16_t   def);
uint16_t nvs_cfg_get_u16  (const char *key, uint16_t  def);
int32_t  nvs_cfg_get_i32  (const char *key, int32_t   def);
float    nvs_cfg_get_float(const char *key, float     def);
void     nvs_cfg_get_str  (const char *key, char *out, size_t max_len,
                            const char *def);

// --- Typed setters (commit to flash immediately after each write) ---
void nvs_cfg_set_u8   (const char *key, uint8_t   val);
void nvs_cfg_set_i16  (const char *key, int16_t   val);
void nvs_cfg_set_u16  (const char *key, uint16_t  val);
void nvs_cfg_set_i32  (const char *key, int32_t   val);
void nvs_cfg_set_float(const char *key, float     val);
void nvs_cfg_set_str  (const char *key, const char *val);

/**
 * Load all NVS settings into g_sensor_state (manual values + modes) and
 * return the configured slave addresses.
 *
 * Call after nvs_cfg_init() and sensor_state_init().  No tasks should be
 * running yet — the mutex is not taken; values are written directly.
 *
 * @param[out] fg_addr_out    FG6485A slave address from NVS (default 1).
 * @param[out] s200_addr_out  S200 slave address from NVS (default 44).
 */
void nvs_cfg_load_all(uint8_t *fg_addr_out, uint8_t *s200_addr_out);
