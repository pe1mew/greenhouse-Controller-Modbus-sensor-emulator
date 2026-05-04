/**
 * @file geo_ip.h
 * @brief IP-based geolocation helper — Phase 10.
 *
 * Queries ip-api.com (HTTP, free tier) to obtain a coarse latitude/longitude
 * from the device's public IP address.  The result is persisted to NVS so
 * that the live_fetch_task can retrieve it even after a restart.
 *
 * This function is called once per new WiFi-STA connection.  If geolocation
 * fails the existing NVS values (default: Amsterdam) are retained.
 */

#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retrieve the device's approximate location from ip-api.com.
 *
 * Performs a synchronous HTTP GET to @c http://ip-api.com/json/ and parses
 * the JSON response.  On success, the values are stored in NVS under
 * @c NVS_KEY_LIVE_LAT / @c NVS_KEY_LIVE_LON and also written to @p lat and
 * @p lon.  On failure the output parameters are left unchanged.
 *
 * Must be called from a task context with an active WiFi-STA connection.
 *
 * @param[out] lat  Latitude in decimal degrees (−90 … +90).
 * @param[out] lon  Longitude in decimal degrees (−180 … +180).
 * @return @c true on success, @c false on network or parse error.
 */
bool geo_ip_get_location(float *lat, float *lon);

#ifdef __cplusplus
}
#endif
