/**
 * @file nvs_config.cpp
 * @brief Persistent settings implementation — Phase 5.
 *
 * Uses the ESP-IDF NVS API directly (available under the Arduino framework).
 * The namespace handle is opened once in nvs_cfg_init() and kept open for
 * the lifetime of the firmware.
 */

#include "nvs_config.h"
#include "../sensors/sensor_state.h"

#include <nvs_flash.h>
#include <nvs.h>
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Module-level NVS handle — opened once, never closed
// ---------------------------------------------------------------------------
static nvs_handle_t s_nvs;
static bool s_nvs_ready = false;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void nvs_cfg_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partition corrupt or schema version changed — erase and re-init.
        Serial.println("[nvs] partition corrupt/version changed — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    // ESP_ERR_INVALID_STATE means nvs_flash_init() was already called by the
    // Arduino framework — treat as success.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[nvs] flash init error: %s\n", esp_err_to_name(err));
        return;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        Serial.printf("[nvs] open error: %s\n", esp_err_to_name(err));
        return;
    }

    s_nvs_ready = true;
    Serial.println("[nvs] namespace 'emulator' opened");
}

// ---------------------------------------------------------------------------
// Typed getters
// ---------------------------------------------------------------------------

uint8_t nvs_cfg_get_u8(const char *key, uint8_t def)
{
    uint8_t v = def;
    if (s_nvs_ready) { nvs_get_u8(s_nvs, key, &v); }
    return v;
}

int16_t nvs_cfg_get_i16(const char *key, int16_t def)
{
    int16_t v = def;
    if (s_nvs_ready) { nvs_get_i16(s_nvs, key, &v); }
    return v;
}

uint16_t nvs_cfg_get_u16(const char *key, uint16_t def)
{
    uint16_t v = def;
    if (s_nvs_ready) { nvs_get_u16(s_nvs, key, &v); }
    return v;
}

int32_t nvs_cfg_get_i32(const char *key, int32_t def)
{
    int32_t v = def;
    if (s_nvs_ready) { nvs_get_i32(s_nvs, key, &v); }
    return v;
}

float nvs_cfg_get_float(const char *key, float def)
{
    float v = def;
    if (s_nvs_ready) {
        size_t sz = sizeof(float);
        nvs_get_blob(s_nvs, key, &v, &sz);
    }
    return v;
}

void nvs_cfg_get_str(const char *key, char *out, size_t max_len, const char *def)
{
    bool got = false;
    if (s_nvs_ready) {
        size_t sz = max_len;
        if (nvs_get_str(s_nvs, key, out, &sz) == ESP_OK) {
            got = true;
        }
    }
    if (!got) {
        strncpy(out, def, max_len - 1);
        out[max_len - 1] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Typed setters — commit after each write so no buffered writes are lost
// ---------------------------------------------------------------------------

void nvs_cfg_set_u8(const char *key, uint8_t val)
{
    if (!s_nvs_ready) { return; }
    nvs_set_u8(s_nvs, key, val);
    nvs_commit(s_nvs);
}

void nvs_cfg_set_i16(const char *key, int16_t val)
{
    if (!s_nvs_ready) { return; }
    nvs_set_i16(s_nvs, key, val);
    nvs_commit(s_nvs);
}

void nvs_cfg_set_u16(const char *key, uint16_t val)
{
    if (!s_nvs_ready) { return; }
    nvs_set_u16(s_nvs, key, val);
    nvs_commit(s_nvs);
}

void nvs_cfg_set_i32(const char *key, int32_t val)
{
    if (!s_nvs_ready) { return; }
    nvs_set_i32(s_nvs, key, val);
    nvs_commit(s_nvs);
}

void nvs_cfg_set_float(const char *key, float val)
{
    if (!s_nvs_ready) { return; }
    nvs_set_blob(s_nvs, key, &val, sizeof(float));
    nvs_commit(s_nvs);
}

void nvs_cfg_set_str(const char *key, const char *val)
{
    if (!s_nvs_ready) { return; }
    nvs_set_str(s_nvs, key, val);
    nvs_commit(s_nvs);
}

// ---------------------------------------------------------------------------
// Load all NVS settings into g_sensor_state
// ---------------------------------------------------------------------------

void nvs_cfg_load_all(uint8_t *fg_addr_out, uint8_t *s200_addr_out)
{
    // --- FG6485A ---
    uint8_t fg_addr = nvs_cfg_get_u8(NVS_KEY_FG_SLAVE_ADDR, 1);
    g_sensor_state.fg_mode        = (sensor_mode_t)nvs_cfg_get_u8(NVS_KEY_FG_MODE,
                                     (uint8_t)SENSOR_MODE_MANUAL);
    g_sensor_state.fg_temperature = nvs_cfg_get_i16(NVS_KEY_FG_TEMP, 250);
    g_sensor_state.fg_humidity    = nvs_cfg_get_u16(NVS_KEY_FG_HUM,  500);

    // --- S200 ---
    uint8_t s200_addr = nvs_cfg_get_u8(NVS_KEY_S200_SLAVE_ADDR, 44);
    g_sensor_state.s200_mode      = (sensor_mode_t)nvs_cfg_get_u8(NVS_KEY_S200_MODE,
                                     (uint8_t)SENSOR_MODE_MANUAL);

    // In manual mode all three direction samples share the same stored value.
    int32_t dir = nvs_cfg_get_i32(NVS_KEY_S200_DIR, 180000);
    g_sensor_state.s200_dir_min = g_sensor_state.s200_dir_max =
        g_sensor_state.s200_dir_avg = dir;

    // In manual mode all three speed samples share the same stored value.
    int32_t spd = nvs_cfg_get_i32(NVS_KEY_S200_SPD, 5000);
    g_sensor_state.s200_spd_min = g_sensor_state.s200_spd_max =
        g_sensor_state.s200_spd_avg = spd;

    // Mirror the configured slave address into the FC03 config register.
    g_sensor_state.s200_slave_addr_reg = s200_addr;

    // --- Live mode lat/lon (used by live_fetch_task in Phase 10) ---
    // Loaded here to verify the blob NVS path works; not yet stored in
    // sensor_state (no field exists until Phase 10).
    float lat = nvs_cfg_get_float(NVS_KEY_LIVE_LAT, 52.37f);
    float lon = nvs_cfg_get_float(NVS_KEY_LIVE_LON,  4.90f);

    // --- Log all loaded values ---
    Serial.printf("[nvs] FG6485A addr=%u  mode=%u  temp=%d  hum=%u\n",
                  fg_addr,
                  (uint8_t)g_sensor_state.fg_mode,
                  (int)g_sensor_state.fg_temperature,
                  (unsigned)g_sensor_state.fg_humidity);
    Serial.printf("[nvs] S200    addr=%u  mode=%u  spd=%ld  dir=%ld\n",
                  s200_addr,
                  (uint8_t)g_sensor_state.s200_mode,
                  (long)spd,
                  (long)dir);
    Serial.printf("[nvs] live    lat=%.3f  lon=%.3f\n", lat, lon);

    char ntp[NVS_STR_MAX_NTP];
    nvs_cfg_get_str(NVS_KEY_NTP_SERVER, ntp, sizeof(ntp), "pool.ntp.org");
    Serial.printf("[nvs] ntp     server=%s\n", ntp);

    char tz[NVS_STR_MAX_TZ];
    nvs_cfg_get_str(NVS_KEY_TZ_POSIX, tz, sizeof(tz), "");
    Serial.printf("[nvs] tz       posix=%s\n", tz[0] ? tz : "(auto)");

    *fg_addr_out   = fg_addr;
    *s200_addr_out = s200_addr;
}
