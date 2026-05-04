/**
 * @file led.h
 * @brief RGB LED (WS2812B on G27) wrapper — Phase 1 HAL.
 *
 * Provides three operations:
 *   led_init()        — initialise FastLED, set steady blue.
 *   led_set(color)    — set LED to color and hold it.
 *   led_blink(color)  — set color for `ms` ms, then return to blue.
 *                       Blocks the calling FreeRTOS task (vTaskDelay).
 *
 * Colour convention (from design):
 *   Blue    (steady)  — normal operation / idle
 *   Green   (blink)   — valid Modbus frame received (correct CRC)
 *   Red     (blink)   — frame received with wrong CRC
 *   Red     (steady)  — faulty operation / unrecoverable error
 */

#pragma once

#include <stdint.h>
#include <FastLED.h>

#define LED_PIN    27
#define NUM_LEDS   1

/** Initialise FastLED and set the LED to steady blue. */
void led_init(void);

/** Set the LED to a fixed color. */
void led_set(CRGB color);

/**
 * Set the LED to color for ms milliseconds, then restore blue.
 * Blocks the calling FreeRTOS task via vTaskDelay — do NOT call from an ISR.
 */
void led_blink(CRGB color, uint16_t ms = 80);
