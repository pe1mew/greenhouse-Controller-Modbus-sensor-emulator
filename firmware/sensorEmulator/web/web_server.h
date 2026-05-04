/**
 * @file web_server.h
 * @brief HTTP server, WebSocket live push, and settings POST handlers — Phase 7.
 *
 * Technology: ESP-IDF httpd (HTTP/1.1 + WebSocket) + SPIFFS for static assets.
 *
 * Endpoints
 * ---------
 * GET  /              → redirect to /index.html
 * GET  /index.html    → single-page app (served from SPIFFS)
 * GET  /style.css     → stylesheet (served from SPIFFS)
 * GET  /app.js        → WebSocket client + Apply logic (served from SPIFFS)
 * GET  /ws            → WebSocket upgrade; server pushes status JSON every 1 s
 *
 * POST /config/sensor   — slave address, mode, manual sensor values → NVS + sensor_state
 *                         Values are clamped to physical sensor range (design §11.1)
 *                         before being applied; clamped values returned in JSON response.
 * POST /config/wifi     — SSID + password → wifi_manager_connect()
 * POST /config/time     — ISO-8601 datetime string → settimeofday()
 * POST /config/ntp      — NTP server hostname → NVS (Phase 9 re-inits SNTP)
 * POST /replay/upload   — CSV file upload (Phase 11 stub — returns 501)
 * POST /replay/control  — start/stop playback (Phase 11 stub — returns 501)
 * POST /log/clear       — flush Modbus log (Phase 12 stub; broadcasts log_clear WS message)
 *
 * WebSocket push payload (every 1 s):
 * @code
 *   {
 *     "type"      : "status",
 *     "fg"        : { "temp": 25.0, "hum": 50.0 },
 *     "s200"      : { "spd": 5.0,   "dir": 180.0 },
 *     "wifi"      : { "mode": "STA", "ip": "192.168.x.y", "rssi": -55 },
 *     "time"      : "2026-05-04T14:30:00",
 *     "ntp_synced": false
 *   }
 * @endcode
 *
 * Static files must be uploaded to SPIFFS before use:
 * @code
 *   pio run -t uploadfs -e sensorEmulator
 * @endcode
 */

#pragma once

/**
 * @brief Mount SPIFFS, start the HTTP server, register all URI handlers, and
 *        launch the WebSocket push task (priority 1, stack 4 KiB).
 *
 * Must be called once from @c setup() after @c wifi_manager_init().
 * Safe to call before any WiFi client connects — the server binds to port 80
 * and is reachable in both AP and STA modes.
 */
void web_server_init(void);
