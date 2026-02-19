/*
 * Race Timer
 *
 * Provides precise elapsed time measurement using the nRF52840's
 * hardware TIMER peripheral. The mainboard is the authoritative
 * source of truth for elapsed time.
 *
 * Uses TIMER0 in 32-bit mode at 1MHz (1µs resolution) for
 * sub-millisecond accuracy.
 */

#ifndef RACE_TIMER_H_
#define RACE_TIMER_H_

#include <zephyr/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the race timer hardware.
 * @return 0 on success, negative errno on failure.
 */
int race_timer_init(void);

/**
 * @brief Start the race timer (clears and begins counting).
 */
void race_timer_start(void);

/**
 * @brief Stop the race timer.
 */
void race_timer_stop(void);

/**
 * @brief Reset the race timer to zero.
 */
void race_timer_reset(void);

/**
 * @brief Get the current elapsed time in milliseconds.
 * @return Elapsed time in ms since race_timer_start() was called.
 */
uint32_t race_timer_get_ms(void);

/**
 * @brief Get the current elapsed time in microseconds.
 * @return Elapsed time in µs since race_timer_start() was called.
 */
uint32_t race_timer_get_us(void);

/**
 * @brief Check if the timer is currently running.
 * @return true if running, false if stopped.
 */
bool race_timer_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* RACE_TIMER_H_ */

