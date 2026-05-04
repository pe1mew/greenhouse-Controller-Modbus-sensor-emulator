/**
 * @file led.cpp
 * @brief RGB LED (WS2812B on G27) wrapper — Phase 1 HAL implementation.
 */

#include "led.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/** @brief FastLED pixel buffer for the single WS2812B LED. */
static CRGB leds[NUM_LEDS];

void led_init(void)
{
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(50);
    led_set(CRGB::Blue);
}

void led_set(CRGB color)
{
    leds[0] = color;
    FastLED.show();
}

void led_blink(CRGB color, uint16_t ms)
{
    led_set(color);
    vTaskDelay(pdMS_TO_TICKS(ms));
    led_set(CRGB::Blue);
}
