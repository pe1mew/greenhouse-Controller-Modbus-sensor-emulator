/**
 * @file geo_ip.cpp
 * @brief IP-based geolocation helper — Phase 10.
 *
 * Queries the free ip-api.com endpoint over plain HTTP (no TLS required on
 * the free tier) to obtain a coarse latitude/longitude from the device's
 * public IP.  Persists the result to NVS.
 */

#include "geo_ip.h"
#include "../config/nvs_config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <cJSON.h>

// ip-api.com free tier; request only the fields we need to minimise payload.
static constexpr const char *GEO_IP_URL =
    "http://ip-api.com/json/?fields=status,lat,lon";

// HTTP connect + response timeout in milliseconds.
static constexpr int GEO_IP_TIMEOUT_MS = 8000;

bool geo_ip_get_location(float *lat, float *lon)
{
    HTTPClient http;
    http.begin(GEO_IP_URL);
    http.setTimeout(GEO_IP_TIMEOUT_MS);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[geo_ip] HTTP %d — geolocation failed\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) {
        Serial.println("[geo_ip] JSON parse error");
        return false;
    }

    bool ok = false;
    cJSON *j_status = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(j_status) &&
        strcmp(j_status->valuestring, "success") == 0)
    {
        cJSON *j_lat = cJSON_GetObjectItem(root, "lat");
        cJSON *j_lon = cJSON_GetObjectItem(root, "lon");
        if (cJSON_IsNumber(j_lat) && cJSON_IsNumber(j_lon)) {
            *lat = (float)j_lat->valuedouble;
            *lon = (float)j_lon->valuedouble;
            nvs_cfg_set_float(NVS_KEY_LIVE_LAT, *lat);
            nvs_cfg_set_float(NVS_KEY_LIVE_LON, *lon);
            Serial.printf("[geo_ip] Location: %.4f, %.4f\n", *lat, *lon);
            ok = true;
        }
    } else {
        Serial.println("[geo_ip] API returned non-success status");
    }

    cJSON_Delete(root);
    return ok;
}
