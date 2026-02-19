/*
 * Race Timer Implementation
 *
 * Uses Zephyr's kernel uptime for millisecond-resolution timing.
 * The nRF52840's k_uptime_get() is based on the RTC peripheral
 * which provides ~30.5µs resolution (32768 Hz clock).
 *
 * For the race timer application, millisecond resolution is more
 * than sufficient. Using kernel uptime avoids conflicts with
 * other peripherals that may use hardware timers.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "race_timer.h"

LOG_MODULE_REGISTER(race_timer, LOG_LEVEL_INF);

static int64_t start_time;
static bool timer_running;

int race_timer_init(void)
{
    start_time = 0;
    timer_running = false;

    LOG_INF("Race timer initialized");
    return 0;
}

void race_timer_start(void)
{
    start_time = k_uptime_get();
    timer_running = true;

    LOG_INF("Race timer started at %lld ms", start_time);
}

void race_timer_stop(void)
{
    timer_running = false;

    LOG_INF("Race timer stopped. Elapsed: %u ms", race_timer_get_ms());
}

void race_timer_reset(void)
{
    start_time = 0;
    timer_running = false;

    LOG_INF("Race timer reset");
}

uint32_t race_timer_get_ms(void)
{
    if (start_time == 0) {
        return 0;
    }

    int64_t now = k_uptime_get();
    int64_t elapsed = now - start_time;

    return (uint32_t)elapsed;
}

uint32_t race_timer_get_us(void)
{
    /* Zephyr's uptime is in milliseconds, so microsecond
     * precision isn't truly available. Return ms * 1000. */
    return race_timer_get_ms() * 1000;
}

bool race_timer_is_running(void)
{
    return timer_running;
}

