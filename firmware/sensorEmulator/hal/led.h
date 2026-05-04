/**
 * @file led.h
 * @brief RGB LED (WS2812B on G27) wrapper — Phase 1 HAL.
 *
 * Provides three operations:
 *   - led_init()   — initialise FastLED, set steady blue.
 *   - led_set()    — set the LED to a colour and hold it.
 *   - led_blink()  — set colour for @p ms milliseconds, then restore blue;
 *                    blocks the calling FreeRTOS task via vTaskDelay.
 *
 * Colour convention (design §2):
 *   | Colour       | Pattern | Meaning                                     |
 *   |:-------------|:--------|:--------------------------------------------|
 *   | Blue         | Steady  | Normal operation / idle                     |
 *   | Green        | Blink   | Valid Modbus frame received (correct CRC)   |
 *   | Red          | Blink   | Frame received with invalid CRC             |
 *   | Red          | Steady  | Faulty operation / unrecoverable error      |
 */

#pragma once

#include <stdint.h>
#include <FastLED.h>

/** @brief GPIO number of the WS2812B data line (Atom Lite built-in LED). */
#define LED_PIN    27

/** @brief Number of WS2812B pixels on the data line (Atom Lite has exactly one). */
#define NUM_LEDS   1

/**
 * @brief Initialise FastLED and set the LED to steady blue.
 *
 * Configures the WS2812B driver on LED_PIN with GRB pixel order and brightness
 * 50.  Must be called once from setup() before any other @c led_* function.
 */
void led_init(void);

/**
 * @brief Set the LED to @p color and hold it indefinitely.
 *
 * Subsequent calls overwrite the previous colour immediately.
 *
 * @param color  CRGB colour value (e.g. @c CRGB::Blue, @c CRGB::Red).
 */
void led_set(CRGB color);

/**
 * @brief Flash the LED with @p color for @p ms milliseconds, then restore blue.
 *
 * Blocks the calling FreeRTOS task via vTaskDelay for the blink duration.
 * Do **not** call from an ISR or from any context that must not block.
 *
 * @param color  CRGB colour to display during the blink.
 * @param ms     Blink duration in milliseconds (default 80 ms).
 */
void led_blink(CRGB color, uint16_t ms = 80);
