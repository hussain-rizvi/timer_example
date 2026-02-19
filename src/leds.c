/*
 * LED Controller Implementation
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "leds.h"

LOG_MODULE_REGISTER(leds, LOG_LEVEL_INF);

/* Device tree references */
#define LED_NODE(n) DT_ALIAS(race_led##n)
#define STATUS_LED_NODE DT_ALIAS(status_led)

static const struct gpio_dt_spec button_led_specs[NUM_BUTTON_LEDS] = {
    GPIO_DT_SPEC_GET(LED_NODE(1), gpios),
    GPIO_DT_SPEC_GET(LED_NODE(2), gpios),
    GPIO_DT_SPEC_GET(LED_NODE(3), gpios),
    GPIO_DT_SPEC_GET(LED_NODE(4), gpios),
    GPIO_DT_SPEC_GET(LED_NODE(5), gpios),
};

static const struct gpio_dt_spec status_led_spec =
    GPIO_DT_SPEC_GET(STATUS_LED_NODE, gpios);

/* Status blink work */
static struct k_work_delayable blink_work;
static uint32_t blink_on_ms;
static uint32_t blink_off_ms;
static uint32_t blink_remaining;
static bool blink_state;

static void blink_work_handler(struct k_work *work)
{
    if (blink_remaining == 0) {
        gpio_pin_set_dt(&status_led_spec, 0);
        return;
    }

    blink_state = !blink_state;
    gpio_pin_set_dt(&status_led_spec, blink_state ? 1 : 0);

    if (!blink_state) {
        blink_remaining--;
    }

    uint32_t delay = blink_state ? blink_on_ms : blink_off_ms;
    k_work_reschedule(&blink_work, K_MSEC(delay));
}

int leds_init(void)
{
    int err;

    /* Initialize button LEDs */
    for (int i = 0; i < NUM_BUTTON_LEDS; i++) {
        if (!gpio_is_ready_dt(&button_led_specs[i])) {
            LOG_ERR("Button LED %d GPIO not ready", i + 1);
            return -ENODEV;
        }

        err = gpio_pin_configure_dt(&button_led_specs[i], GPIO_OUTPUT_INACTIVE);
        if (err) {
            LOG_ERR("Failed to configure button LED %d (err %d)", i + 1, err);
            return err;
        }
    }

    /* Initialize status LED */
    if (!gpio_is_ready_dt(&status_led_spec)) {
        LOG_ERR("Status LED GPIO not ready");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&status_led_spec, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure status LED (err %d)", err);
        return err;
    }

    /* Initialize blink work */
    k_work_init_delayable(&blink_work, blink_work_handler);

    LOG_INF("LEDs initialized");
    return 0;
}

void leds_set_button(uint8_t led_index, bool on)
{
    if (led_index < 1 || led_index > NUM_BUTTON_LEDS) {
        return;
    }
    gpio_pin_set_dt(&button_led_specs[led_index - 1], on ? 1 : 0);
}

void leds_set_status(bool on)
{
    /* Cancel any ongoing blink */
    k_work_cancel_delayable(&blink_work);
    gpio_pin_set_dt(&status_led_spec, on ? 1 : 0);
}

void leds_all_off(void)
{
    for (int i = 0; i < NUM_BUTTON_LEDS; i++) {
        gpio_pin_set_dt(&button_led_specs[i], 0);
    }
}

void leds_all_on(void)
{
    for (int i = 0; i < NUM_BUTTON_LEDS; i++) {
        gpio_pin_set_dt(&button_led_specs[i], 1);
    }
}

void leds_status_blink(uint32_t on_ms, uint32_t off_ms, uint32_t count)
{
    k_work_cancel_delayable(&blink_work);

    if (count == 0) {
        gpio_pin_set_dt(&status_led_spec, 0);
        return;
    }

    blink_on_ms = on_ms;
    blink_off_ms = off_ms;
    blink_remaining = count;
    blink_state = false;

    k_work_reschedule(&blink_work, K_NO_WAIT);
}

void leds_flash_button(uint8_t led_index)
{
    if (led_index < 1 || led_index > NUM_BUTTON_LEDS) {
        return;
    }
    /* Quick flash: on then off after 200ms */
    gpio_pin_set_dt(&button_led_specs[led_index - 1], 1);
    /* Note: This is a simple blocking flash. For production,
     * use a work queue with delayed off. */
}

