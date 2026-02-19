/*
 * Button Handler
 *
 * Manages the 5 physical race buttons (4 contestant + 1 auxiliary).
 * Uses GPIO interrupts for precise timing of button press events.
 */

#ifndef BUTTONS_H_
#define BUTTONS_H_

#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUM_RACE_BUTTONS  4  /* Buttons 1-4 for contestants */
#define NUM_TOTAL_BUTTONS 5  /* Including button 5 (auxiliary) */

/**
 * @brief Callback invoked when a race button is pressed.
 *
 * @param button_index Button number (1-5).
 * @param timestamp_ms Kernel uptime in milliseconds at time of press.
 */
typedef void (*button_press_cb_t)(uint8_t button_index, uint32_t timestamp_ms);

/**
 * @brief Initialize all race button GPIOs with interrupt handling.
 *
 * @param callback Function called on button press events.
 * @return 0 on success, negative errno on failure.
 */
int buttons_init(button_press_cb_t callback);

/**
 * @brief Enable button interrupts (call before race start).
 */
void buttons_enable(void);

/**
 * @brief Disable button interrupts (call when race not active).
 */
void buttons_disable(void);

/**
 * @brief Check if a specific button is currently pressed.
 *
 * @param button_index Button number (1-5).
 * @return true if button is currently pressed (active), false otherwise.
 */
bool buttons_is_pressed(uint8_t button_index);

#ifdef __cplusplus
}
#endif

#endif /* BUTTONS_H_ */

