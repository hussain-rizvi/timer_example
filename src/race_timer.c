/*
 * Race Timer Implementation
 *
 * Uses Zephyr kernel uptime for elapsed-time measurement.
 * The timer latches the last elapsed value when stopped so that reads after
 * stop() remain stable instead of continuing to grow.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "race_timer.h"

LOG_MODULE_REGISTER(race_timer, LOG_LEVEL_INF);

static int64_t start_time_ms;
static uint32_t latched_elapsed_ms;
static bool timer_running;

int race_timer_init(void)
{
    start_time_ms = 0;
    latched_elapsed_ms = 0;
    timer_running = false;

    LOG_INF("Race timer initialized");
    return 0;
}

void race_timer_start(void)
{
    start_time_ms = k_uptime_get();
    latched_elapsed_ms = 0;
    timer_running = true;

    LOG_INF("Race timer started at %lld ms", start_time_ms);
}

void race_timer_stop(void)
{
    if (timer_running) {
        int64_t now_ms = k_uptime_get();
        int64_t elapsed_ms = now_ms - start_time_ms;

        if (elapsed_ms < 0) {
            elapsed_ms = 0;
        }

        latched_elapsed_ms = (uint32_t)elapsed_ms;
        timer_running = false;
    }

    LOG_INF("Race timer stopped. Elapsed: %u ms", latched_elapsed_ms);
}

void race_timer_reset(void)
{
    start_time_ms = 0;
    latched_elapsed_ms = 0;
    timer_running = false;

    LOG_INF("Race timer reset");
}

uint32_t race_timer_get_ms(void)
{
    if (timer_running) {
        int64_t now_ms = k_uptime_get();
        int64_t elapsed_ms = now_ms - start_time_ms;

        if (elapsed_ms < 0) {
            return 0;
        }

        return (uint32_t)elapsed_ms;
    }

    return latched_elapsed_ms;
}

uint32_t race_timer_get_us(void)
{
    /* The firmware's source clock is millisecond-based, so report the ms value
     * in microseconds for API compatibility. */
    return race_timer_get_ms() * 1000U;
}

bool race_timer_is_running(void)
{
    return timer_running;
}
