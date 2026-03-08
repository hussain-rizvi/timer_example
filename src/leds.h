/*
 * LED Controller
 *
 * Controls the 5 button LEDs and the status LED.
 *
 * Pin Mapping (from schematic):
 *   LED 1 (J3): P1.07 (OUT_LED2) - Race button 1 LED
 *   LED 2 (J4): P1.05 (OUT_LED3) - Race button 2 LED
 *   LED 3 (J5): P1.03 (OUT_LED4) - Race button 3 LED
 *   LED 4 (J6): P1.01 (OUT_LED5) - Race button 4 LED
 *   LED 5 (J2): P1.06 (OUT_LED1) - Manual start/stop LED (auxiliary)
 *   Status:     P0.06 (STATUS)   - Active HIGH → Blue LED → R20 → GND
 */

#ifndef LEDS_H_
#define LEDS_H_

#include <zephyr/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUM_BUTTON_LEDS 5

/**
 * @brief Initialize all LED GPIOs.
 * @return 0 on success, negative errno on failure.
 */
int leds_init(void);

/**
 * @brief Set a button LED on or off.
 * @param led_index LED number (1-5).
 * @param on true = LED on, false = LED off.
 */
void leds_set_button(uint8_t led_index, bool on);

/**
 * @brief Set the status LED on or off.
 * @param on true = LED on, false = LED off.
 */
void leds_set_status(bool on);

/**
 * @brief Turn off all button LEDs.
 */
void leds_all_off(void);

/**
 * @brief Turn on all button LEDs.
 */
void leds_all_on(void);

/**
 * @brief Blink the status LED (non-blocking, uses work queue).
 * @param on_ms  On duration in milliseconds.
 * @param off_ms Off duration in milliseconds.
 * @param count  Number of blinks (0 = blink forever). Use leds_set_status() to stop.
 */
void leds_status_blink(uint32_t on_ms, uint32_t off_ms, uint32_t count);

/**
 * @brief Blink a specific button LED continuously (non-blocking, work queue).
 * @param led_index LED number (1-5).
 * @param on_ms  On duration in milliseconds.
 * @param off_ms Off duration in milliseconds.
 */
void leds_blink_button(uint8_t led_index, uint32_t on_ms, uint32_t off_ms);

/**
 * @brief Stop any ongoing button LED blink and turn the LED off.
 */
void leds_stop_blink_button(void);

#ifdef __cplusplus
}
#endif

#endif /* LEDS_H_ */

