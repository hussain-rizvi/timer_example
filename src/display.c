/*
 * TLC5926 Daisy-Chain Display Driver Implementation
 *
 * See display.h for the chain wiring and bit-layout documentation.
 *
 * Update sequence on every display change:
 *   1. SPI write of 4 bytes (32 bits) — first byte goes deepest into the
 *      chain and ends up in Seg A; last byte stays in Seg B.
 *   2. Pulse LE high → low. While LE is high the output latches are
 *      transparent to the shift register; the falling edge latches the
 *      new values so the outputs hold them until the next update.
 *
 * Brightness is controlled by PWMing the active-LOW OE pin via the
 * pwm-leds (LED) API. The DT entry uses PWM_POLARITY_INVERTED so that an
 * LED-API brightness of 100 % corresponds to OE held LOW (always enabled).
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "display.h"

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

#define TLC5926_SPI_FREQUENCY_HZ 1000000U

/*
 * 7-segment font. Bit mapping matches the TLC5926 output ordering on each
 * segment board:
 *   bit 0 = A (top)        bit 4 = E (lower left)
 *   bit 1 = B (upper right) bit 5 = F (upper left)
 *   bit 2 = C (lower right) bit 6 = G (middle)
 *   bit 3 = D (bottom)
 */
static const uint8_t SEGMENT_FONT_DIGIT[] = {
    0x3F, /* 0 */
    0x06, /* 1 */
    0x5B, /* 2 */
    0x4F, /* 3 */
    0x66, /* 4 */
    0x6D, /* 5 */
    0x7D, /* 6 */
    0x07, /* 7 */
    0x7F, /* 8 */
    0x6F, /* 9 */
};

#define SEG_CHAR_d 0x5EU /* lowercase d */
#define SEG_CHAR_O 0x3FU
#define SEG_CHAR_n 0x54U /* lowercase n */
#define SEG_CHAR_E 0x79U
#define SEG_BLANK  0x00U

/* ─────────────────────────────────────────────────────────────────────────
 * BOARD WIRING / CALIBRATION
 *
 * If the digits show up in the wrong physical order on the clock, change
 * the four mapping macros below. They pick which logical clock digit
 * (0 = rightmost, 3 = leftmost; same indexing as display_time() output)
 * is driven by each of the four TLC5926 digit positions in the chain.
 *
 * Position naming:
 *   SEG_A_LEFT_DIGIT  = TLC5926 outputs OUT7..OUT13 on Segment A board
 *   SEG_A_RIGHT_DIGIT = TLC5926 outputs OUT0..OUT6  on Segment A board
 *   SEG_B_LEFT_DIGIT  = TLC5926 outputs OUT7..OUT13 on Segment B board
 *   SEG_B_RIGHT_DIGIT = TLC5926 outputs OUT0..OUT6  on Segment B board
 *
 * Default assumes Seg A is the LEFT board of the clock and that within
 * each board OUT7..13 ("left digit") drives the higher-order digit.
 * Common fixes if the display is wrong:
 *
 *   "12:34 shows as 34:12"  (boards swapped left↔right):
 *       swap SEG_A_LEFT/RIGHT values with SEG_B_LEFT/RIGHT values
 *
 *   "12:34 shows as 21:43"  (left↔right reversed within each board):
 *       swap SEG_A_LEFT_DIGIT ↔ SEG_A_RIGHT_DIGIT
 *       swap SEG_B_LEFT_DIGIT ↔ SEG_B_RIGHT_DIGIT
 *
 *   "12:34 shows as 4321 fully mirrored":
 *       reverse all four values (3→0, 2→1, 1→2, 0→3)
 * ───────────────────────────────────────────────────────────────────────── */
#define DISPLAY_SEG_A_LEFT_DIGIT   3U  /* leftmost digit on the clock */
#define DISPLAY_SEG_A_RIGHT_DIGIT  2U
#define DISPLAY_SEG_B_LEFT_DIGIT   1U
#define DISPLAY_SEG_B_RIGHT_DIGIT  0U  /* rightmost digit on the clock */

/* Bit position of the colon driver within the Seg B 16-bit word
 * (Seg B OUT14). User-confirmed mapping; flip to 15 if hardware
 * actually drives the colon from OUT15 instead. */
#define SEG_B_COLON_BIT 14U

/* SPI controller (no child device, no CS — TLC5926 uses LE instead). */
static const struct device *spi_bus = DEVICE_DT_GET(DT_NODELABEL(spi1));
static const struct spi_config spi_cfg = {
    .frequency = TLC5926_SPI_FREQUENCY_HZ,
    .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .slave     = 0U,
    /* .cs left zero-initialized → no CS GPIO is toggled. */
};

/* LE latch pin (P0.24). */
static const struct gpio_dt_spec le_gpio =
    GPIO_DT_SPEC_GET(DT_NODELABEL(display_le), gpios);

/* Brightness PWM (P0.25 via pwm-leds, single channel index 0). */
static const struct device *brightness_dev =
    DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(display_brightness)));
#define BRIGHTNESS_LED_INDEX 0U
#define BRIGHTNESS_DEFAULT_PCT 50U

static bool display_initialized;
static uint32_t chain_cache;
static bool chain_cache_valid;

/*
 * Pack a 32-bit chain word from four logical clock-digit patterns + colon.
 * digits[i] is the 7-segment pattern for clock position i (0 = rightmost,
 * 3 = leftmost). The four DISPLAY_SEG_*_DIGIT macros pick which clock
 * position lands at each TLC5926 output slot.
 */
static uint32_t build_chain_word(const uint8_t digits[DISPLAY_NUM_DIGITS],
                                 bool colon)
{
    uint16_t seg_a =
        ((uint16_t)(digits[DISPLAY_SEG_A_LEFT_DIGIT]  & 0x7F) << 7) |
         (uint16_t)(digits[DISPLAY_SEG_A_RIGHT_DIGIT] & 0x7F);

    uint16_t seg_b =
        ((uint16_t)(digits[DISPLAY_SEG_B_LEFT_DIGIT]  & 0x7F) << 7) |
         (uint16_t)(digits[DISPLAY_SEG_B_RIGHT_DIGIT] & 0x7F);
    if (colon) {
        seg_b |= (uint16_t)(1U << SEG_B_COLON_BIT);
    }

    /* Seg A goes out FIRST (MSBs of 32-bit word) because it sits farther down
     * the chain — the first bits shifted in get pushed all the way through. */
    return ((uint32_t)seg_a << 16) | (uint32_t)seg_b;
}

/* Write one 32-bit chain word and pulse LE to latch outputs. */
static int chain_write(uint32_t word)
{
    int err;

    if (chain_cache_valid && chain_cache == word) {
        return 0;
    }

    uint8_t tx_buf[4] = {
        (uint8_t)(word >> 24),
        (uint8_t)(word >> 16),
        (uint8_t)(word >> 8),
        (uint8_t)(word),
    };
    struct spi_buf spi_tx = { .buf = tx_buf, .len = sizeof(tx_buf) };
    struct spi_buf_set tx = { .buffers = &spi_tx, .count = 1 };

    err = spi_write(spi_bus, &spi_cfg, &tx);
    if (err) {
        chain_cache_valid = false;
        return err;
    }

    /* Pulse LE: high transfers the shift register into the output latches;
     * the falling edge holds the new values. nRF52840 GPIO writes are slow
     * enough (>20 ns) that no explicit delay is needed for TLC5926. */
    gpio_pin_set_dt(&le_gpio, 1);
    gpio_pin_set_dt(&le_gpio, 0);

    chain_cache = word;
    chain_cache_valid = true;
    return 0;
}

int display_set_brightness(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    if (!device_is_ready(brightness_dev)) {
        return -ENODEV;
    }
    return led_set_brightness(brightness_dev, BRIGHTNESS_LED_INDEX, percent);
}

int display_init(void)
{
    int err;

    display_initialized = false;
    chain_cache_valid = false;

    if (!device_is_ready(spi_bus)) {
        LOG_ERR("SPI1 not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&le_gpio)) {
        LOG_ERR("Display LE GPIO not ready");
        return -ENODEV;
    }
    if (!device_is_ready(brightness_dev)) {
        LOG_ERR("Display brightness PWM not ready");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&le_gpio, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("LE GPIO configure failed (err %d)", err);
        return err;
    }

    /* Start with the display blanked, then enable outputs at full brightness. */
    {
        const uint8_t blank[DISPLAY_NUM_DIGITS] = { SEG_BLANK, SEG_BLANK,
                                                    SEG_BLANK, SEG_BLANK };
        err = chain_write(build_chain_word(blank, false));
    }
    if (err) {
        LOG_ERR("Initial chain blank failed (err %d)", err);
        return err;
    }

    err = display_set_brightness(BRIGHTNESS_DEFAULT_PCT);
    if (err) {
        LOG_ERR("Brightness init failed (err %d)", err);
        return err;
    }

    display_initialized = true;
    LOG_INF("TLC5926 daisy-chain display initialized");
    return 0;
}

void display_time(uint32_t time_ms)
{
    uint8_t digits[DISPLAY_NUM_DIGITS];

    if (!display_initialized) {
        return;
    }

    uint32_t total_seconds = time_ms / 1000U;
    uint32_t minutes = total_seconds / 60U;
    uint8_t seconds = (uint8_t)(total_seconds % 60U);

    if (minutes > 99U) {
        minutes = 99U;
    }

    digits[3] = SEGMENT_FONT_DIGIT[(minutes / 10U) % 10U];
    digits[2] = SEGMENT_FONT_DIGIT[minutes % 10U];
    digits[1] = SEGMENT_FONT_DIGIT[(seconds / 10U) % 10U];
    digits[0] = SEGMENT_FONT_DIGIT[seconds % 10U];

    (void)chain_write(build_chain_word(digits, /* colon = */ true));
}

void display_clear(void)
{
    const uint8_t blank[DISPLAY_NUM_DIGITS] = { SEG_BLANK, SEG_BLANK,
                                                SEG_BLANK, SEG_BLANK };

    if (!display_initialized) {
        return;
    }
    (void)chain_write(build_chain_word(blank, false));
}

void display_done(void)
{
    /* "dOnE", left → right. Indices follow display position: 3 = leftmost. */
    const uint8_t done_digits[DISPLAY_NUM_DIGITS] = {
        [3] = SEG_CHAR_d,
        [2] = SEG_CHAR_O,
        [1] = SEG_CHAR_n,
        [0] = SEG_CHAR_E,
    };

    if (!display_initialized) {
        return;
    }
    (void)chain_write(build_chain_word(done_digits, false));
}
