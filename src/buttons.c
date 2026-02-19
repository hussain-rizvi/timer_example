/*
 * Button Handler Implementation
 *
 * GPIO interrupt-driven button detection for 5 physical race buttons.
 * Hardware: External buttons via JST connectors (J2-J6) with 10k pull-ups
 *           to 3V3. Active LOW when pressed.
 *
 * Pin Mapping (from schematic):
 *   Button 1 (J3): P1.15 (OUT_SW2) - Race contestant 1
 *   Button 2 (J4): P0.29 (OUT_SW3) - Race contestant 2
 *   Button 3 (J5): P0.31 (OUT_SW4) - Race contestant 3
 *   Button 4 (J6): P0.30 (OUT_SW5) - Race contestant 4
 *   Button 5 (J2): P1.13 (OUT_SW1) - Manual start/stop (auxiliary)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "buttons.h"

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

/* Debounce time in milliseconds */
#define DEBOUNCE_MS 50

/* Device tree node references for buttons */
#define BTN_NODE(n) DT_ALIAS(race_btn##n)

static const struct gpio_dt_spec button_specs[NUM_TOTAL_BUTTONS] = {
    GPIO_DT_SPEC_GET(BTN_NODE(1), gpios),
    GPIO_DT_SPEC_GET(BTN_NODE(2), gpios),
    GPIO_DT_SPEC_GET(BTN_NODE(3), gpios),
    GPIO_DT_SPEC_GET(BTN_NODE(4), gpios),
    GPIO_DT_SPEC_GET(BTN_NODE(5), gpios),
};

static struct gpio_callback button_cb_data[NUM_TOTAL_BUTTONS];
static button_press_cb_t user_callback;
static int64_t last_press_time[NUM_TOTAL_BUTTONS];
static bool buttons_active;

/* Work item for deferred processing (called from ISR context) */
struct button_work_data {
    struct k_work work;
    uint8_t button_index;
    uint32_t timestamp_ms;
};

static struct button_work_data btn_work[NUM_TOTAL_BUTTONS];

static void button_work_handler(struct k_work *work)
{
    struct button_work_data *data = CONTAINER_OF(work, struct button_work_data, work);

    if (user_callback && buttons_active) {
        user_callback(data->button_index, data->timestamp_ms);
    }
}

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    int64_t now = k_uptime_get();

    /* Determine which button triggered the interrupt */
    for (int i = 0; i < NUM_TOTAL_BUTTONS; i++) {
        if (dev == button_specs[i].port &&
            (pins & BIT(button_specs[i].pin))) {

            /* Debounce check */
            if ((now - last_press_time[i]) < DEBOUNCE_MS) {
                return;
            }
            last_press_time[i] = now;

            LOG_INF("Button %d pressed at %lld ms", i + 1, now);

            /* Submit work for deferred callback execution */
            btn_work[i].button_index = i + 1;  /* 1-indexed */
            btn_work[i].timestamp_ms = (uint32_t)now;
            k_work_submit(&btn_work[i].work);
            return;
        }
    }
}

int buttons_init(button_press_cb_t callback)
{
    int err;

    user_callback = callback;
    buttons_active = false;

    for (int i = 0; i < NUM_TOTAL_BUTTONS; i++) {
        if (!gpio_is_ready_dt(&button_specs[i])) {
            LOG_ERR("Button %d GPIO device not ready", i + 1);
            return -ENODEV;
        }

        err = gpio_pin_configure_dt(&button_specs[i], GPIO_INPUT);
        if (err) {
            LOG_ERR("Failed to configure button %d GPIO (err %d)", i + 1, err);
            return err;
        }

        /* Initialize work items */
        k_work_init(&btn_work[i].work, button_work_handler);

        /* Configure interrupt on falling edge (active LOW buttons) */
        err = gpio_pin_interrupt_configure_dt(&button_specs[i],
                                               GPIO_INT_EDGE_TO_ACTIVE);
        if (err) {
            LOG_ERR("Failed to configure button %d interrupt (err %d)", i + 1, err);
            return err;
        }

        gpio_init_callback(&button_cb_data[i], button_isr,
                          BIT(button_specs[i].pin));
        err = gpio_add_callback(button_specs[i].port, &button_cb_data[i]);
        if (err) {
            LOG_ERR("Failed to add button %d callback (err %d)", i + 1, err);
            return err;
        }

        /* Start with interrupts disabled */
        gpio_pin_interrupt_configure_dt(&button_specs[i], GPIO_INT_DISABLE);

        last_press_time[i] = 0;

        LOG_INF("Button %d initialized", i + 1);
    }

    LOG_INF("All buttons initialized");
    return 0;
}

void buttons_enable(void)
{
    buttons_active = true;

    for (int i = 0; i < NUM_TOTAL_BUTTONS; i++) {
        gpio_pin_interrupt_configure_dt(&button_specs[i],
                                         GPIO_INT_EDGE_TO_ACTIVE);
    }

    LOG_INF("Button interrupts enabled");
}

void buttons_disable(void)
{
    buttons_active = false;

    for (int i = 0; i < NUM_TOTAL_BUTTONS; i++) {
        gpio_pin_interrupt_configure_dt(&button_specs[i], GPIO_INT_DISABLE);
    }

    LOG_INF("Button interrupts disabled");
}

bool buttons_is_pressed(uint8_t button_index)
{
    if (button_index < 1 || button_index > NUM_TOTAL_BUTTONS) {
        return false;
    }

    /* gpio_pin_get_dt returns 1 when active (button pressed) due to GPIO_ACTIVE_LOW flag */
    return gpio_pin_get_dt(&button_specs[button_index - 1]) == 1;
}

