/**
 * @file main.cpp
 * @brief Modbus Sensor Emulator — Phase 7: Web Interface (HTTP + WebSocket).
 *
 * Adds an HTTP/WebSocket server served from SPIFFS.  The web UI provides
 * live status updates via WebSocket and POST endpoints for sensor config,
 * WiFi credentials, NTP server, and manual time setting.
 *
 * Boot sequence:
 *   1. nvs_cfg_init()          — open "emulator" NVS namespace
 *   2. sensor_state_init()     — set hardcoded defaults + create mutex
 *   3. nvs_cfg_load_all()      — overwrite defaults with NVS-stored values
 *   4. Register Modbus handlers; start modbus_slave_task
 *   5. wifi_manager_init()     — start AP + STA FSM task
 *   6. web_server_init()       — mount SPIFFS, start httpd + WebSocket push
 *
 * FG6485A (slave addr from NVS, default 1):
 *   FC03 0x0000–0x0001  → humidity + temperature (×10 encoding)
 *   FC03 0x0008–0x000B  → static device info
 *   FC03 0x000C–0x0013  → alarm config
 *   FC16 0x000C–0x001E  → alarm config + correction offsets (write)
 *
 * S200 (slave addr from NVS, default 44):
 *   FC04 0x0008–0x0013  → wind direction/speed min/max/avg (int32 ×1000)
 *   FC04 0x001C–0x001F  → heating temperature (int32 ×1000)
 *   FC03 0x1000–0x1001  → slave address + baud rate config
 *
 * LED:
 *   Blue  (steady) — idle, waiting for a frame.
 *   Green (blink)  — frame received with valid CRC.
 *   Red   (blink)  — frame received with invalid CRC.
 *
 * Hardware:
 *   M5Stack Atom Lite (ESP32-PICO-D4) + Atomic RS485 Base
 *   G27 — RGB LED (FastLED WS2812B GRB)
 *   G22 — UART2 RX  ← RS485 RO
 *   G19 — UART2 TX  → RS485 DI  (direction auto-controlled by hardware)
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "hal/led.h"
#include "hal/rs485.h"
#include "modbus/modbus_slave.h"
#include "sensors/sensor_state.h"
#include "sensors/fg6485a_slave.h"
#include "sensors/s200_slave.h"
#include "config/nvs_config.h"
#include "wifi/wifi_manager.h"
#include "web/web_server.h"

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    led_init();    // Sets LED blue immediately.
    rs485_init();

    // Initialise NVS before anything reads or writes settings.
    nvs_cfg_init();

    delay(100);    // Allow peripherals to settle.

    Serial.println();
    Serial.println("================================================");
    Serial.println("  Modbus Sensor Emulator — Phase 7 Web Interface");
    Serial.println("  M5Stack Atom Lite + Atomic RS485 Base");
    Serial.println("  9600 baud 8N1  |  LED G27  |  RX G22  TX G19");
    Serial.println("================================================");

    // Initialise shared state with hardcoded defaults and create mutex.
    sensor_state_init();

    // Override defaults with any values stored in NVS; retrieve slave addrs.
    uint8_t fg_addr   = 1;
    uint8_t s200_addr = 44;
    nvs_cfg_load_all(&fg_addr, &s200_addr);

    // Register sensor handlers using the NVS-configured slave addresses.
    fg6485a_slave_register(fg_addr);
    s200_slave_register(s200_addr);

    // Configure address filter for the Modbus slave task.
    modbus_slave_set_addrs(fg_addr, s200_addr);

    // Start Modbus slave at highest FreeRTOS priority.
    xTaskCreate(modbus_slave_task, "modbus_slave", 4096, nullptr,
                configMAX_PRIORITIES - 1, nullptr);

    // Start WiFi manager (AP + STA FSM + mDNS).
    wifi_manager_init();

    // Start HTTP server + WebSocket push (mounts SPIFFS).
    web_server_init();
}

void loop()
{
    // All work is done in FreeRTOS tasks; keep loop() yielding.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
