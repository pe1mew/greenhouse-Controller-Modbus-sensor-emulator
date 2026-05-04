/**
 * @file web_server.cpp
 * @brief HTTP server + WebSocket live push + settings POST handlers — Phase 7.
 *
 * Architecture
 * ------------
 * - ESP-IDF httpd (esp_http_server.h) on port 80.
 * - SPIFFS holds static web assets (index.html, style.css, app.js).
 * - A FreeRTOS task (ws_push_task) calls httpd_queue_work every 1 s; the
 *   queued callback iterates active WebSocket connections and pushes a JSON
 *   status frame using httpd_ws_send_frame_async.
 * - POST handlers parse JSON bodies with cJSON, clamp sensor values against
 *   the physical-range constants in sensor_state.h, and write to both
 *   g_sensor_state (under mutex) and NVS.
 */

#include "web_server.h"

#include "../sensors/sensor_state.h"
#include "../config/nvs_config.h"
#include "../wifi/wifi_manager.h"
#include "../modbus/modbus_slave.h"
#include "../sensors/fg6485a_slave.h"
#include "../sensors/s200_slave.h"
#include "../tasks/fg6485a_mode_task.h"
#include "../tasks/s200_mode_task.h"
#include "../tasks/ntp_task.h"
#include "../tasks/live_fetch_task.h"
#include "../tasks/replay_task.h"
#include "../modbus/modbus_log.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>
#include <cmath>
#include <cstring>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** @brief WebSocket push interval in milliseconds. */
static constexpr size_t  WS_PUSH_INTERVAL_MS = 1000;
/** @brief Stack size in bytes for the WS push task. */
static constexpr size_t  WS_PUSH_STACK        = 4096;
/** @brief Maximum HTTP request body size in bytes. */
static constexpr size_t  HTTP_BODY_MAX         = 512;
/** @brief Maximum size of a status JSON frame in bytes. */
static constexpr size_t  STATUS_JSON_MAX       = 768;
/** @brief Maximum number of simultaneous WebSocket clients. */
static constexpr size_t  MAX_WS_CLIENTS        = 8;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

/** @cond INTERNAL */
static httpd_handle_t s_server    = nullptr;
static uint8_t        s_fg_addr   = 1;
static uint8_t        s_s200_addr = 44;
/** @endcond */

// ---------------------------------------------------------------------------
// Clamping helpers
// ---------------------------------------------------------------------------

/** @brief Clamp integer @p v to the closed range [@p lo, @p hi]. */
static inline int clamp_int(int v, int lo, int hi)
    { return v < lo ? lo : v > hi ? hi : v; }

/** @brief Clamp @p v to the int16_t range [@p lo, @p hi]. */
static inline int16_t clamp_i16(int v, int16_t lo, int16_t hi)
    { return (int16_t)clamp_int(v, (int)lo, (int)hi); }

/** @brief Clamp @p v to the uint16_t range [@p lo, @p hi]. */
static inline uint16_t clamp_u16(int v, uint16_t lo, uint16_t hi)
    { return (uint16_t)clamp_int(v, (int)lo, (int)hi); }

/** @brief Clamp @p v to the int32_t range [@p lo, @p hi]. */
static inline int32_t clamp_i32(int64_t v, int32_t lo, int32_t hi)
    { return (int32_t)(v < lo ? lo : v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// HTTP body reader
// ---------------------------------------------------------------------------

/**
 * @brief Accumulate the full HTTP POST body into @p buf.
 *
 * Reads up to @p buf_size − 1 bytes and null-terminates the result.
 *
 * @param req       Incoming HTTP request.
 * @param buf       Destination buffer.
 * @param buf_size  Size of @p buf in bytes.
 * @return ESP_OK on success, ESP_FAIL if a receive chunk returns an error.
 */
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len == 0) { buf[0] = '\0'; return ESP_OK; }
    size_t to_read = req->content_len < buf_size - 1
                     ? req->content_len : buf_size - 1;
    size_t done = 0;
    while (done < to_read) {
        int r = httpd_req_recv(req, buf + done, to_read - done);
        if (r <= 0) return ESP_FAIL;
        done += (size_t)r;
    }
    buf[done] = '\0';
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SPIFFS file server
// ---------------------------------------------------------------------------

/**
 * @brief Serve a file from SPIFFS as a chunked HTTP response.
 *
 * Sends a 404 if the file does not exist or a 500 if it cannot be opened.
 *
 * @param req           Incoming HTTP request.
 * @param path          Absolute SPIFFS path (e.g. "/index.html").
 * @param content_type  MIME type string applied to the response.
 * @return ESP_OK in all cases; HTTP error codes are sent to the client.
 */
static esp_err_t serve_spiffs(httpd_req_t *req,
                               const char  *path,
                               const char  *content_type)
{
    if (!SPIFFS.exists(path)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }
    File f = SPIFFS.open(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open error");
        return ESP_OK;
    }
    httpd_resp_set_type(req, content_type);
    uint8_t chunk[512];
    size_t  n;
    while ((n = f.read(chunk, sizeof(chunk))) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)chunk, (ssize_t)n) != ESP_OK) {
            break;
        }
    }
    f.close();
    httpd_resp_send_chunk(req, nullptr, 0);  // end chunked response
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Static file GET handlers
// ---------------------------------------------------------------------------

/** @brief Redirect GET "/" → serve /index.html. @param req HTTP request. */
static esp_err_t handle_root(httpd_req_t *req)
{
    return serve_spiffs(req, "/index.html", "text/html");
}

/** @brief Serve GET /index.html from SPIFFS. @param req HTTP request. */
static esp_err_t handle_html(httpd_req_t *req)
    { return serve_spiffs(req, "/index.html", "text/html"); }

/** @brief Serve GET /style.css from SPIFFS. @param req HTTP request. */
static esp_err_t handle_css(httpd_req_t *req)
    { return serve_spiffs(req, "/style.css",  "text/css"); }

/** @brief Serve GET /app.js from SPIFFS. @param req HTTP request. */
static esp_err_t handle_js(httpd_req_t *req)
    { return serve_spiffs(req, "/app.js", "application/javascript"); }

// ---------------------------------------------------------------------------
// WebSocket broadcast via httpd_queue_work
// ---------------------------------------------------------------------------

/**
 * @brief Payload buffer passed from ws_push_task to broadcast_cb via httpd_queue_work.
 */
struct broadcast_ctx_t {
    uint8_t payload[STATUS_JSON_MAX]; /**< @brief JSON frame bytes to broadcast. */
    size_t  len;                      /**< @brief Number of valid bytes in @c payload. */
};

/**
 * @brief Send the JSON frame in @p arg to every active WebSocket client.
 *
 * Invoked on the httpd task via httpd_queue_work().  Iterates all open file
 * descriptors, filters for WebSocket connections, and calls
 * httpd_ws_send_frame_async() for each.  Frees @p arg when done.
 *
 * @param arg  Heap-allocated @ref broadcast_ctx_t containing the JSON payload.
 */
static void broadcast_cb(void *arg)
{
    auto *ctx = static_cast<broadcast_ctx_t *>(arg);

    size_t n = MAX_WS_CLIENTS;
    int    fds[MAX_WS_CLIENTS];

    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        httpd_ws_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = ctx->payload;
        frame.len     = ctx->len;
        frame.final   = true;

        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) ==
                    HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(s_server, fds[i], &frame);
            }
        }
    }
    free(ctx);
}

/**
 * @brief Allocate a broadcast_ctx_t, copy @p json into it, and schedule
 *        broadcast_cb via httpd_queue_work().
 *
 * Safe to call from any FreeRTOS task.  The ctx is freed by broadcast_cb.
 *
 * @param json  Null-terminated JSON string to broadcast.
 * @param len   Length of @p json (excluding the null terminator).
 */
static void ws_broadcast(const char *json, size_t len)
{
    if (!s_server || len == 0) return;
    auto *ctx = static_cast<broadcast_ctx_t *>(malloc(sizeof(broadcast_ctx_t)));
    if (!ctx) return;
    if (len >= STATUS_JSON_MAX) len = STATUS_JSON_MAX - 1;
    memcpy(ctx->payload, json, len);
    ctx->payload[len] = '\0';
    ctx->len = len;
    if (httpd_queue_work(s_server, broadcast_cb, ctx) != ESP_OK) free(ctx);
}

// ---------------------------------------------------------------------------
// Dynamic (heap-string) WebSocket broadcast — used for variable-length log frames
// ---------------------------------------------------------------------------

/**
 * @brief Context for a dynamic-length WS broadcast.
 *
 * Unlike broadcast_ctx_t which embeds a fixed-size payload, this structure
 * holds a heap-allocated string so that log JSON frames of arbitrary size
 * can be dispatched without a fixed upper bound.
 */
struct broadcast_dyn_ctx_t {
    char   *json; /**< @brief Heap-allocated JSON string (freed by callback). */
    size_t  len;  /**< @brief Length of @c json. */
};

/**
 * @brief Broadcast callback for dynamic-length payloads.
 *
 * Invoked on the httpd task via httpd_queue_work().  Sends the JSON to all
 * active WebSocket clients, then frees the payload and the context.
 *
 * @param arg  Heap-allocated @ref broadcast_dyn_ctx_t.
 */
static void broadcast_dyn_cb(void *arg)
{
    auto *ctx = static_cast<broadcast_dyn_ctx_t *>(arg);

    size_t n = MAX_WS_CLIENTS;
    int    fds[MAX_WS_CLIENTS];

    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        httpd_ws_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = reinterpret_cast<uint8_t *>(ctx->json);
        frame.len     = ctx->len;
        frame.final   = true;

        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) ==
                    HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(s_server, fds[i], &frame);
            }
        }
    }
    free(ctx->json);
    free(ctx);
}

/**
 * @brief Schedule a heap-allocated JSON string for WebSocket broadcast.
 *
 * Takes ownership of @p json (calls free() on it in all paths).
 * Safe to call from any FreeRTOS task.
 *
 * @param json  Null-terminated JSON string (heap-allocated via cJSON_PrintUnformatted).
 */
static void ws_broadcast_dyn(char *json)
{
    if (!s_server || !json) { free(json); return; }
    auto *ctx = static_cast<broadcast_dyn_ctx_t *>(malloc(sizeof(broadcast_dyn_ctx_t)));
    if (!ctx) { free(json); return; }
    ctx->json = json;
    ctx->len  = strlen(json);
    if (httpd_queue_work(s_server, broadcast_dyn_cb, ctx) != ESP_OK) {
        free(json);
        free(ctx);
    }
}

// ---------------------------------------------------------------------------
// Status JSON builder
// ---------------------------------------------------------------------------

/**
 * @brief Snapshot g_sensor_state and WiFi info, then serialise to a JSON string.
 *
 * The produced object has the shape documented in web_server.h under
 * "WebSocket push payload".  The sensor values are converted to engineering
 * units (raw ÷10 for FG6485A, raw ÷1000 for S200).
 *
 * @param buf       Destination buffer for the null-terminated JSON string.
 * @param buf_size  Size of @p buf in bytes.
 */
static void build_status_json(char *buf, size_t buf_size)
{
    // Snapshot sensor state under mutex.
    xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
    int16_t       fg_temp   = g_sensor_state.fg_temperature;
    uint16_t      fg_hum    = g_sensor_state.fg_humidity;
    sensor_mode_t fg_mode   = g_sensor_state.fg_mode;
    int32_t       spd_avg   = g_sensor_state.s200_spd_avg;
    int32_t       dir_avg   = g_sensor_state.s200_dir_avg;
    int32_t       s200_heat = g_sensor_state.s200_heat_high;
    sensor_mode_t s200_mode = g_sensor_state.s200_mode;
    xSemaphoreGive(g_sensor_state.mutex);

    // WiFi info.
    wifi_manager_state_t wst = wifi_manager_get_state();
    char ip[20] = "";
    if (wst == WIFI_STATE_STA) {
        wifi_manager_get_sta_ip(ip, sizeof(ip));
    } else {
        strncpy(ip, "192.168.4.1", sizeof(ip) - 1);
    }
    const char *mode_str = (wst == WIFI_STATE_STA)        ? "STA"
                         : (wst == WIFI_STATE_CONNECTING) ? "Connecting"
                                                           : "AP";
    int32_t rssi = (wst == WIFI_STATE_STA) ? WiFi.RSSI() : 0;

    // System time.
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    char time_str[32] = "";
    // Only format if clock appears set (> 2020-01-01).
    if (now > 1577836800LL) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", &ti);
    }

    // Build JSON.
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");

    cJSON *fg = cJSON_CreateObject();
    cJSON_AddNumberToObject(fg, "temp", fg_temp / 10.0);
    cJSON_AddNumberToObject(fg, "hum",  fg_hum  / 10.0);
    cJSON_AddNumberToObject(fg, "mode", (int)fg_mode);
    cJSON_AddNumberToObject(fg, "addr", s_fg_addr);
    cJSON_AddItemToObject(root, "fg", fg);

    cJSON *s2 = cJSON_CreateObject();
    cJSON_AddNumberToObject(s2, "spd",  spd_avg   / 1000.0);
    cJSON_AddNumberToObject(s2, "dir",  dir_avg   / 1000.0);
    cJSON_AddNumberToObject(s2, "heat", s200_heat / 1000.0);
    cJSON_AddNumberToObject(s2, "mode", (int)s200_mode);
    cJSON_AddNumberToObject(s2, "addr", s_s200_addr);
    cJSON_AddItemToObject(root, "s200", s2);

    cJSON *wf = cJSON_CreateObject();
    cJSON_AddStringToObject(wf, "mode", mode_str);
    cJSON_AddStringToObject(wf, "ip",   ip);
    cJSON_AddNumberToObject(wf, "rssi", rssi);
    {
        String sta_ssid = (wst == WIFI_STATE_STA) ? WiFi.SSID() : String("");
        cJSON_AddStringToObject(wf, "ssid", sta_ssid.c_str());
    }
    cJSON_AddItemToObject(root, "wifi", wf);

    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddBoolToObject(root, "ntp_synced", ntp_is_synced() ? 1 : 0);

    cJSON *lv = cJSON_CreateObject();
    cJSON_AddNumberToObject(lv, "lat",
        nvs_cfg_get_float(NVS_KEY_LIVE_LAT, 52.37f));
    cJSON_AddNumberToObject(lv, "lon",
        nvs_cfg_get_float(NVS_KEY_LIVE_LON,  4.90f));
    cJSON_AddItemToObject(root, "live", lv);

    cJSON_AddNumberToObject(root, "live_fetch_age", live_fetch_get_age_s());
    cJSON_AddBoolToObject(root, "live_fetch_ok",  live_fetch_was_ok() ? 1 : 0);

    replay_state_t rs = replay_task_get_state();
    const char *rs_str = (rs == REPLAY_RUNNING) ? "running"
                       : (rs == REPLAY_DONE)    ? "done"
                       : (rs == REPLAY_ERROR)   ? "error"
                                                : "idle";
    cJSON *rpl = cJSON_CreateObject();
    cJSON_AddStringToObject(rpl, "state", rs_str);
    cJSON_AddNumberToObject(rpl, "row",   replay_task_get_row());
    cJSON_AddItemToObject(root, "replay", rpl);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (str) {
        strncpy(buf, str, buf_size - 1);
        buf[buf_size - 1] = '\0';
        free(str);
    } else {
        strncpy(buf, "{\"type\":\"status\"}", buf_size - 1);
    }
}

// ---------------------------------------------------------------------------
// WebSocket endpoint handler
// ---------------------------------------------------------------------------

/**
 * @brief Handle GET /ws — accept the WebSocket upgrade, then discard incoming frames.
 *
 * The server is push-only in Phase 7: ws_push_task owns all outgoing frames.
 * Incoming text frames are received and silently discarded.
 *
 * @param req  Incoming HTTP/WebSocket request.
 * @return ESP_OK.
 */
static esp_err_t handle_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Initial HTTP → WebSocket upgrade handshake.
        Serial.println("[web] WebSocket client connected");
        return ESP_OK;
    }
    // Receive and discard incoming frames (push-only in Phase 7).
    uint8_t           buf[128] = {};
    httpd_ws_frame_t  frame;
    memset(&frame, 0, sizeof(frame));
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = buf;
    httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /config/sensor
// ---------------------------------------------------------------------------

/**
 * @brief Handle POST /config/sensor — update slave address, mode, and/or
 *        manual sensor values for the FG6485A or S200.
 *
 * JSON body keys (all optional except @c sensor):
 * @code
 * { "sensor":"fg6485a"|"s200",
 *   "addr":int,                         // Modbus slave address 1–247
 *   "mode":0|1|2,                       // SENSOR_MODE_AUTO/MANUAL/REPLAY
 *   "temp":float,   "hum":float,        // FG6485A: °C, %RH
 *   "spd":float,    "dir":float,        // S200: m/s, degrees
 *   "heat":float }                      // S200: °C
 * @endcode
 *
 * Values are clamped to the physical sensor range (design §11.1).
 * The response echoes accepted keys with their clamped values and sets
 * @c "clamped":true if any value was adjusted.
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK; HTTP 400 is sent to the client on parse error.
 */
static esp_err_t handle_post_sensor(httpd_req_t *req)
{
    char body[HTTP_BODY_MAX];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_OK;
    }

    cJSON *j_sensor = cJSON_GetObjectItem(root, "sensor");
    if (!j_sensor || !cJSON_IsString(j_sensor)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing sensor");
        return ESP_OK;
    }

    const char *sensor  = j_sensor->valuestring;
    bool        clamped = false;
    cJSON      *resp    = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "sensor", sensor);

    if (strcmp(sensor, "fg6485a") == 0) {

        cJSON *j_addr = cJSON_GetObjectItem(root, "addr");
        cJSON *j_mode = cJSON_GetObjectItem(root, "mode");
        cJSON *j_temp = cJSON_GetObjectItem(root, "temp");
        cJSON *j_hum  = cJSON_GetObjectItem(root, "hum");

        if (cJSON_IsNumber(j_addr)) {
            uint8_t addr = (uint8_t)clamp_int(j_addr->valueint, 1, 247);
            nvs_cfg_set_u8(NVS_KEY_FG_SLAVE_ADDR, addr);
            fg6485a_slave_register(addr);
            modbus_slave_set_addrs(addr, s_s200_addr);
            s_fg_addr = addr;
            cJSON_AddNumberToObject(resp, "addr", addr);
        }

        if (cJSON_IsNumber(j_mode)) {
            sensor_mode_t mode = (sensor_mode_t)clamp_int(j_mode->valueint, 0, 2);
            nvs_cfg_set_u8(NVS_KEY_FG_MODE, (uint8_t)mode);
            xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
            g_sensor_state.fg_mode = mode;
            xSemaphoreGive(g_sensor_state.mutex);
        }

        if (cJSON_IsNumber(j_temp)) {
            int16_t raw = (int16_t)roundf((float)j_temp->valuedouble * 10.0f);
            int16_t cr  = clamp_i16((int)raw, FG6485A_TEMP_RAW_MIN,
                                               FG6485A_TEMP_RAW_MAX);
            if (cr != raw) clamped = true;
            nvs_cfg_set_i16(NVS_KEY_FG_TEMP, cr);
            xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
            g_sensor_state.fg_temperature = cr;
            xSemaphoreGive(g_sensor_state.mutex);
            cJSON_AddNumberToObject(resp, "temp", cr / 10.0);
        }

        if (cJSON_IsNumber(j_hum)) {
            int32_t raw32  = (int32_t)roundf((float)j_hum->valuedouble * 10.0f);
            uint16_t cr    = clamp_u16(raw32, FG6485A_HUM_RAW_MIN,
                                              FG6485A_HUM_RAW_MAX);
            if ((int32_t)cr != raw32) clamped = true;
            nvs_cfg_set_u16(NVS_KEY_FG_HUM, cr);
            xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
            g_sensor_state.fg_humidity = cr;
            xSemaphoreGive(g_sensor_state.mutex);
            cJSON_AddNumberToObject(resp, "hum", cr / 10.0);
        }

        fg6485a_mode_task_notify();

    } else if (strcmp(sensor, "s200") == 0) {

        cJSON *j_addr = cJSON_GetObjectItem(root, "addr");
        cJSON *j_mode = cJSON_GetObjectItem(root, "mode");
        cJSON *j_spd  = cJSON_GetObjectItem(root, "spd");
        cJSON *j_dir  = cJSON_GetObjectItem(root, "dir");
        cJSON *j_heat = cJSON_GetObjectItem(root, "heat");

        if (cJSON_IsNumber(j_addr)) {
            uint8_t addr = (uint8_t)clamp_int(j_addr->valueint, 1, 247);
            nvs_cfg_set_u8(NVS_KEY_S200_SLAVE_ADDR, addr);
            s200_slave_register(addr);
            modbus_slave_set_addrs(s_fg_addr, addr);
            s_s200_addr = addr;
            cJSON_AddNumberToObject(resp, "addr", addr);
        }

        if (cJSON_IsNumber(j_mode)) {
            sensor_mode_t mode = (sensor_mode_t)clamp_int(j_mode->valueint, 0, 2);
            nvs_cfg_set_u8(NVS_KEY_S200_MODE, (uint8_t)mode);
            xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
            g_sensor_state.s200_mode = mode;
            xSemaphoreGive(g_sensor_state.mutex);
        }

        if (cJSON_IsNumber(j_spd)) {
            int32_t raw = (int32_t)roundf((float)j_spd->valuedouble * 1000.0f);
            int32_t cr  = clamp_i32(raw, S200_SPD_RAW_MIN, S200_SPD_RAW_MAX);
            if (cr != raw) clamped = true;
            nvs_cfg_set_i32(NVS_KEY_S200_SPD, cr);
            xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
            g_sensor_state.s200_spd_min = g_sensor_state.s200_spd_max =
                g_sensor_state.s200_spd_avg = cr;
            xSemaphoreGive(g_sensor_state.mutex);
            cJSON_AddNumberToObject(resp, "spd", cr / 1000.0);
        }

        if (cJSON_IsNumber(j_dir)) {
            int32_t raw = (int32_t)roundf((float)j_dir->valuedouble * 1000.0f);
            int32_t cr  = clamp_i32(raw, S200_DIR_RAW_MIN, S200_DIR_RAW_MAX);
            if (cr != raw) clamped = true;
            nvs_cfg_set_i32(NVS_KEY_S200_DIR, cr);
            xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
            g_sensor_state.s200_dir_min = g_sensor_state.s200_dir_max =
                g_sensor_state.s200_dir_avg = cr;
            xSemaphoreGive(g_sensor_state.mutex);
            cJSON_AddNumberToObject(resp, "dir", cr / 1000.0);
        }

        if (cJSON_IsNumber(j_heat)) {
            int32_t raw = (int32_t)roundf((float)j_heat->valuedouble * 1000.0f);
            int32_t cr  = clamp_i32(raw, S200_HEAT_RAW_MIN, S200_HEAT_RAW_MAX);
            if (cr != raw) clamped = true;
            // Heating temp has no NVS key in Phase 7 — runtime only.
            xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY);
            g_sensor_state.s200_heat_high = g_sensor_state.s200_heat_low = cr;
            xSemaphoreGive(g_sensor_state.mutex);
            cJSON_AddNumberToObject(resp, "heat", cr / 1000.0);
        }

        s200_mode_task_notify();
    }

    cJSON_AddBoolToObject(resp, "clamped", clamped ? 1 : 0);
    cJSON_Delete(root);

    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str ? resp_str : "{}");
    free(resp_str);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /config/wifi
// ---------------------------------------------------------------------------

/**
 * @brief Handle POST /config/wifi — trigger a Wi-Fi STA connection attempt.
 *
 * JSON body: @code { "ssid":"<network>", "pass":"<password>" } @endcode
 *
 * Delegates to wifi_manager_connect(); the outcome is reflected in subsequent
 * WebSocket status pushes (wifi.mode, wifi.ip).
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK; HTTP 400 is sent to the client on parse error.
 */
static esp_err_t handle_post_wifi(httpd_req_t *req)
{
    char body[HTTP_BODY_MAX];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_OK;
    }
    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(j_ssid) && cJSON_IsString(j_pass)) {
        wifi_manager_connect(j_ssid->valuestring, j_pass->valuestring);
        Serial.printf("[web] WiFi connect requested: ssid=%s\n",
                      j_ssid->valuestring);
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /config/time
// ---------------------------------------------------------------------------

/**
 * @brief Handle POST /config/time — set the system clock via settimeofday().
 *
 * JSON body: @code { "time":"2026-05-04T14:30" } @endcode
 * Accepts the @c datetime-local HTML format (YYYY-MM-DDTHH:MM) and the
 * extended form with seconds (YYYY-MM-DDTHH:MM:SS).
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK; HTTP 400 is sent to the client on parse error.
 */
static esp_err_t handle_post_time(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_OK;
    }
    cJSON *j_time = cJSON_GetObjectItem(root, "time");
    if (cJSON_IsString(j_time)) {
        struct tm ti;
        memset(&ti, 0, sizeof(ti));
        // datetime-local yields "YYYY-MM-DDTHH:MM"; also accept ":SS" suffix.
        const char *fmt = strchr(j_time->valuestring, ':') &&
                          strrchr(j_time->valuestring, ':') !=
                          strchr(j_time->valuestring, ':')
                          ? "%Y-%m-%dT%H:%M:%S"
                          : "%Y-%m-%dT%H:%M";
        if (strptime(j_time->valuestring, fmt, &ti)) {
            ti.tm_isdst = -1;  // let mktime determine DST
            time_t t = mktime(&ti);
            if (t != (time_t)-1) {
                struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                settimeofday(&tv, nullptr);
                Serial.printf("[web] Time set: %s\n", j_time->valuestring);
            }
        }
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /config/ntp
// ---------------------------------------------------------------------------

/**
 * @brief Handle POST /config/ntp — persist the NTP server hostname to NVS.
 *
 * JSON body: @code { "server":"pool.ntp.org" } @endcode
 * Phase 9 will read this key and reinitialise SNTP with the saved hostname.
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK; HTTP 400 is sent to the client on parse error.
 */
static esp_err_t handle_post_ntp(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_OK;
    }
    cJSON *j_srv = cJSON_GetObjectItem(root, "server");
    if (cJSON_IsString(j_srv)) {
        char buf[NVS_STR_MAX_NTP];
        strncpy(buf, j_srv->valuestring, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        nvs_cfg_set_str(NVS_KEY_NTP_SERVER, buf);
        Serial.printf("[web] NTP server saved: %s\n", buf);
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /config/tz
// ---------------------------------------------------------------------------

/**
 * @brief Handle POST /config/tz — apply and persist a POSIX TZ string.
 *
 * JSON body: @code { "tz":"CET-1CEST,M3.5.0,M10.5.0/3" } @endcode
 *
 * An empty @c tz value is accepted and stored (ntp_task will fall back to
 * "UTC0" on next boot); the live setenv call in that case uses "UTC0".
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK; HTTP 400 is sent on parse error.
 */
static esp_err_t handle_post_tz(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_OK;
    }
    cJSON *j_tz = cJSON_GetObjectItem(root, "tz");
    const char *applied = "UTC0";
    if (cJSON_IsString(j_tz)) {
        char buf[NVS_STR_MAX_TZ];
        strncpy(buf, j_tz->valuestring, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        applied = (buf[0] != '\0') ? buf : "UTC0";
        setenv("TZ", applied, 1);
        tzset();
        nvs_cfg_set_str(NVS_KEY_TZ_POSIX, buf);
        Serial.printf("[web] TZ applied: %s\n", applied);
    }
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", 1);
    cJSON_AddStringToObject(resp, "tz", applied);
    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str ? resp_str : "{\"ok\":true}");
    free(resp_str);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /config/location
// ---------------------------------------------------------------------------

/**
 * @brief Handle POST /config/location — persist lat/lon for Live-mode fetch.
 *
 * JSON body: @code { "lat": 52.37, "lon": 4.90 } @endcode
 *
 * Values are clamped to valid geographic ranges (lat ±90, lon ±180) and
 * stored in NVS.  The live_fetch_task is notified to pick up the new
 * coordinates on its next cycle.
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK; HTTP 400 is sent on parse error.
 */
static esp_err_t handle_post_location(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_OK;
    }
    cJSON *j_lat = cJSON_GetObjectItem(root, "lat");
    cJSON *j_lon = cJSON_GetObjectItem(root, "lon");
    if (!cJSON_IsNumber(j_lat) || !cJSON_IsNumber(j_lon)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing lat/lon");
        return ESP_OK;
    }
    // Clamp to valid geographic ranges.
    float lat = (float)j_lat->valuedouble;
    float lon = (float)j_lon->valuedouble;
    if (lat < -90.0f)  lat = -90.0f;
    if (lat >  90.0f)  lat =  90.0f;
    if (lon < -180.0f) lon = -180.0f;
    if (lon >  180.0f) lon =  180.0f;
    cJSON_Delete(root);

    nvs_cfg_set_float(NVS_KEY_LIVE_LAT, lat);
    nvs_cfg_set_float(NVS_KEY_LIVE_LON, lon);
    live_fetch_task_notify();   // Wake fetch task to use new coordinates.

    Serial.printf("[web] Location set: %.4f, %.4f\n", lat, lon);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", 1);
    cJSON_AddNumberToObject(resp, "lat", lat);
    cJSON_AddNumberToObject(resp, "lon", lon);
    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str ? resp_str : "{\"ok\":true}");
    free(resp_str);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Phase 11/12 stubs
// ---------------------------------------------------------------------------

/**
 * @brief Drain and discard all remaining bytes in the HTTP request body.
 *
 * Called by stub handlers before sending a response to prevent the httpd
 * pipeline from stalling on an unconsumed body.
 *
 * @param req  Incoming HTTP request whose body should be discarded.
 */
static void drain_body(httpd_req_t *req)
{
    char discard[64];
    while (httpd_req_recv(req, discard, sizeof(discard)) > 0) {}
}

/** @brief Maximum replay CSV upload size in bytes. */
static constexpr size_t REPLAY_MAX_UPLOAD_BYTES = 200 * 1024;

/**
 * @brief Handle POST /replay/upload — stream the request body to SPIFFS.
 *
 * Accepts the CSV as a raw POST body (Content-Type: text/csv or
 * application/octet-stream).  Replaces any existing /replay.csv.
 * Stores the path in NVS and stops any running replay task.
 *
 * Response JSON: @code { "ok":true, "size":<bytes> } @endcode
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK.
 */
static esp_err_t handle_replay_upload(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    if (req->content_len > REPLAY_MAX_UPLOAD_BYTES) {
        drain_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large (max 200 KB)");
        return ESP_OK;
    }

    // Stop any running replay before overwriting the file.
    replay_task_stop();
    vTaskDelay(pdMS_TO_TICKS(100));  // allow task to see the stop flag

    // Remove stale file so SPIFFS reclaims the space.
    if (SPIFFS.exists("/replay.csv")) {
        SPIFFS.remove("/replay.csv");
    }

    File f = SPIFFS.open("/replay.csv", "w");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Cannot create /replay.csv");
        return ESP_OK;
    }

    char   chunk[256];
    int    total     = 0;
    int    remaining = (int)req->content_len;
    bool   write_err = false;

    while (remaining > 0 && !write_err) {
        int to_read  = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
        int received = httpd_req_recv(req, chunk, to_read);
        if (received <= 0) break;
        size_t written = f.write((uint8_t *)chunk, (size_t)received);
        if (written < (size_t)received) {
            write_err = true;
            Serial.println("[web/replay] SPIFFS write failed");
        }
        total     += received;
        remaining -= received;
    }
    f.close();

    if (write_err) {
        SPIFFS.remove("/replay.csv");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Disk write error");
        return ESP_OK;
    }

    nvs_cfg_set_str(NVS_KEY_REPLAY_FILE, "/replay.csv");
    Serial.printf("[web/replay] Uploaded %d bytes → /replay.csv\n", total);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok",   1);
    cJSON_AddNumberToObject(resp, "size", total);
    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str ? resp_str : "{\"ok\":true}");
    free(resp_str);
    return ESP_OK;
}

/**
 * @brief Handle POST /replay/control — start or stop playback.
 *
 * JSON body: @code { "action": "start" | "stop" } @endcode
 *
 * Response JSON: @code { "ok":true, "state":"running|idle" } @endcode
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK.
 */
static esp_err_t handle_replay_control(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_OK;
    }

    cJSON *j_action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(j_action)) {
        if (strcmp(j_action->valuestring, "start") == 0) {
            replay_task_start();
            Serial.println("[web/replay] Start requested");
        } else if (strcmp(j_action->valuestring, "stop") == 0) {
            replay_task_stop();
            Serial.println("[web/replay] Stop requested");
        }
    }
    cJSON_Delete(root);

    replay_state_t rs = replay_task_get_state();
    const char *rs_str = (rs == REPLAY_RUNNING) ? "running" : "idle";

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp,   "ok",    1);
    cJSON_AddStringToObject(resp, "state", rs_str);
    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str ? resp_str : "{\"ok\":true}");
    free(resp_str);
    return ESP_OK;
}

/**
 * @brief Handle POST /log/clear — broadcast a @c log_clear WebSocket event.
 *
 * Drains the request body (no JSON payload expected), then broadcasts
 * @c {"type":"log_clear"} to all connected clients so the GUI table resets.
 * Phase 12 will also flush the in-memory log queue here.
 *
 * @param req  Incoming HTTP POST request.
 * @return ESP_OK.
 */
static esp_err_t handle_post_log_clear(httpd_req_t *req)
{
    drain_body(req);
    modbus_log_clear();
    const char *msg = "{\"type\":\"log_clear\"}";
    ws_broadcast(msg, strlen(msg));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// WebSocket push task
// ---------------------------------------------------------------------------

/**
 * @brief FreeRTOS task: build and broadcast a status JSON frame every 1 s.
 *
 * Runs for the lifetime of the application.  Stack size is WS_PUSH_STACK
 * bytes.  The task blocks on vTaskDelay between broadcasts, so it does not
 * busy-wait.  If @c s_server is not yet ready the iteration is skipped.
 * The FreeRTOS task parameter is unused.
 */
static void ws_push_task(void * /*arg*/)
{
    char json[STATUS_JSON_MAX];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WS_PUSH_INTERVAL_MS));
        if (!s_server) continue;

        // Push periodic status JSON.
        build_status_json(json, sizeof(json));
        ws_broadcast(json, strlen(json));

        // Drain all pending Modbus log entries and forward each as a WS frame.
        log_entry_t entry;
        while (modbus_log_receive(&entry, 0)) {
            // Format timestamp (fallback to empty string if clock not set).
            char ts_buf[32] = "";
            if (entry.ts > 1577836800LL) {   // > 2020-01-01
                struct tm tm_info;
                localtime_r(&entry.ts, &tm_info);
                strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
            }

            // Build hex string: "AA BB CC ..."
            // Reserve 3 chars per byte ("XX ") plus NUL.
            char hex_buf[256 * 3 + 1];
            size_t hex_pos = 0;
            for (uint8_t i = 0; i < entry.len; i++) {
                int written = snprintf(hex_buf + hex_pos,
                                       sizeof(hex_buf) - hex_pos,
                                       (i + 1 < entry.len) ? "%02X " : "%02X",
                                       entry.frame[i]);
                if (written > 0) hex_pos += (size_t)written;
            }

            // Serialise as JSON and hand to the dynamic broadcast helper.
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type",    "log");
            cJSON_AddStringToObject(msg, "ts",      ts_buf);
            cJSON_AddStringToObject(msg, "dir",     entry.dir == LOG_DIR_RX ? "RX" : "TX");
            cJSON_AddStringToObject(msg, "hex",     hex_buf);
            cJSON_AddStringToObject(msg, "summary", entry.summary);
            char *msg_str = cJSON_PrintUnformatted(msg);
            cJSON_Delete(msg);
            ws_broadcast_dyn(msg_str);   // takes ownership; handles nullptr gracefully
        }
    }
}

// ---------------------------------------------------------------------------
// web_server_init
// ---------------------------------------------------------------------------

void web_server_init(void)
{
    // Mount SPIFFS — format on first boot if needed.
    if (!SPIFFS.begin(true)) {
        Serial.println("[web] SPIFFS mount failed");
        return;
    }
    Serial.printf("[web] SPIFFS mounted — total %u B, used %u B\n",
                  (unsigned)SPIFFS.totalBytes(),
                  (unsigned)SPIFFS.usedBytes());

    // Read current slave addresses so POST handlers can update them.
    s_fg_addr   = nvs_cfg_get_u8(NVS_KEY_FG_SLAVE_ADDR, 1);
    s_s200_addr = nvs_cfg_get_u8(NVS_KEY_S200_SLAVE_ADDR, 44);

    // Configure and start httpd.
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 18;
    cfg.max_open_sockets  = 7;
    cfg.lru_purge_enable  = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        Serial.println("[web] httpd start failed");
        return;
    }

    // -----------------------------------------------------------------------
    // GET handlers — static files
    // -----------------------------------------------------------------------
    const httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = handle_root
    };
    const httpd_uri_t u_html = {
        .uri = "/index.html", .method = HTTP_GET, .handler = handle_html
    };
    const httpd_uri_t u_css = {
        .uri = "/style.css", .method = HTTP_GET, .handler = handle_css
    };
    const httpd_uri_t u_js = {
        .uri = "/app.js", .method = HTTP_GET, .handler = handle_js
    };

    // -----------------------------------------------------------------------
    // GET /ws — WebSocket endpoint
    // -----------------------------------------------------------------------
    const httpd_uri_t u_ws = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = handle_ws,
        .user_ctx     = nullptr,
        .is_websocket = true
    };

    // -----------------------------------------------------------------------
    // POST handlers
    // -----------------------------------------------------------------------
    const httpd_uri_t u_sensor = {
        .uri = "/config/sensor", .method = HTTP_POST,
        .handler = handle_post_sensor
    };
    const httpd_uri_t u_wifi = {
        .uri = "/config/wifi", .method = HTTP_POST,
        .handler = handle_post_wifi
    };
    const httpd_uri_t u_time = {
        .uri = "/config/time", .method = HTTP_POST,
        .handler = handle_post_time
    };
    const httpd_uri_t u_ntp = {
        .uri = "/config/ntp", .method = HTTP_POST,
        .handler = handle_post_ntp
    };
    const httpd_uri_t u_tz = {
        .uri = "/config/tz", .method = HTTP_POST,
        .handler = handle_post_tz
    };
    const httpd_uri_t u_location = {
        .uri = "/config/location", .method = HTTP_POST,
        .handler = handle_post_location
    };
    const httpd_uri_t u_replay_up = {
        .uri = "/replay/upload", .method = HTTP_POST,
        .handler = handle_replay_upload
    };
    const httpd_uri_t u_replay_ctrl = {
        .uri = "/replay/control", .method = HTTP_POST,
        .handler = handle_replay_control
    };
    const httpd_uri_t u_log_clear = {
        .uri = "/log/clear", .method = HTTP_POST,
        .handler = handle_post_log_clear
    };

    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_html);
    httpd_register_uri_handler(s_server, &u_css);
    httpd_register_uri_handler(s_server, &u_js);
    httpd_register_uri_handler(s_server, &u_ws);
    httpd_register_uri_handler(s_server, &u_sensor);
    httpd_register_uri_handler(s_server, &u_wifi);
    httpd_register_uri_handler(s_server, &u_time);
    httpd_register_uri_handler(s_server, &u_ntp);
    httpd_register_uri_handler(s_server, &u_tz);
    httpd_register_uri_handler(s_server, &u_location);
    httpd_register_uri_handler(s_server, &u_replay_up);
    httpd_register_uri_handler(s_server, &u_replay_ctrl);
    httpd_register_uri_handler(s_server, &u_log_clear);

    // Launch the WebSocket push task.
    xTaskCreate(ws_push_task, "ws_push", WS_PUSH_STACK, nullptr, 1, nullptr);

    Serial.println("[web] HTTP server listening on port 80");
}
