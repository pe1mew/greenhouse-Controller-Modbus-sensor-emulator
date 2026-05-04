/**
 * @file main.cpp
 * @brief Modbus Sensor Emulator — Phase 2: Modbus Slave Skeleton.
 *
 * Starts the Modbus slave task, which:
 *   - Listens on RS485 for RTU frames (UART2, G22 RX, G19 TX, 9600 baud 8N1).
 *   - Validates each frame's CRC-16/IBM.
 *   - Responds to slave addresses 1 (FG6485A) and 44 (S200).
 *   - Returns exception 0x01 (Illegal Function) for any FC until Phase 3/4
 *     register real handlers.
 *   - Silently ignores broadcast (addr 0x00) and frames for other addresses.
 *
 * LED:
 *   Blue  (steady) — idle, waiting for a frame.
 *   Green (blink)  — frame received with valid CRC (response sent).
 *   Red   (blink)  — frame received with invalid CRC (discarded).
 *
 * USB serial (115200 baud) logs every received and transmitted frame.
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

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    led_init();    // Sets LED blue immediately.
    rs485_init();

    delay(100);    // Allow peripherals to settle.

    Serial.println();
    Serial.println("================================================");
    Serial.println("  Modbus Sensor Emulator — Phase 2 Slave");
    Serial.println("  M5Stack Atom Lite + Atomic RS485 Base");
    Serial.println("  9600 baud 8N1  |  LED G27  |  RX G22  TX G19");
    Serial.println("  Slave addrs: 1 (FG6485A)  44 (S200)");
    Serial.println("================================================");

    // Configure slave addresses (defaults match Phase 3/4 sensor targets).
    modbus_slave_set_addrs(1, 44);

    // Start Modbus slave at highest FreeRTOS priority.
    xTaskCreate(modbus_slave_task, "modbus_slave", 4096, nullptr,
                configMAX_PRIORITIES - 1, nullptr);
}

void loop()
{
    // All work is done in FreeRTOS tasks; keep loop() yielding.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
