/*
 * MAX7221 Seven-Segment Display Driver Implementation
 *
 * The MAX7221 is controlled via SPI. It has a 16-bit shift register:
 *   Byte 0: register address
 *   Byte 1: register data
 *
 * This design uses the MAX7221 in no-decode mode to drive a discrete
 * seven-segment assembly through external segment/digit transistor stages.
 * The MAX7221 retains the digit register contents internally, so the display
 * does not need to be refreshed continuously from firmware.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "display.h"

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/* MAX7221 Register Addresses */
#define MAX7221_REG_NOOP         0x00
#define MAX7221_REG_DIGIT0       0x01
#define MAX7221_REG_DIGIT1       0x02
#define MAX7221_REG_DIGIT2       0x03
#define MAX7221_REG_DIGIT3       0x04
#define MAX7221_REG_DIGIT4       0x05
#define MAX7221_REG_DIGIT5       0x06
#define MAX7221_REG_DIGIT6       0x07
#define MAX7221_REG_DIGIT7       0x08
#define MAX7221_REG_DECODE_MODE  0x09
#define MAX7221_REG_INTENSITY    0x0A
#define MAX7221_REG_SCAN_LIMIT   0x0B
#define MAX7221_REG_SHUTDOWN     0x0C
#define MAX7221_REG_DISPLAY_TEST 0x0F

#define MAX7221_ACTIVE_DIGITS        4U
#define MAX7221_SCAN_LIMIT_4_DIGITS  0x03U
#define MAX7221_DEFAULT_INTENSITY    0x00U
#define MAX7221_SPI_FREQUENCY_HZ     1000000U

/*
 * Raw segment font (no-decode mode).
 * Bit mapping: DP G F E D C B A  (bit 7 down to bit 0)
 */
static const uint8_t SEGMENT_FONT[] = {
    0x7E,  /* 0 */
    0x30,  /* 1 */
    0x6D,  /* 2 */
    0x79,  /* 3 */
    0x33,  /* 4 */
    0x5B,  /* 5 */
    0x5F,  /* 6 */
    0x70,  /* 7 */
    0x7F,  /* 8 */
    0x7B,  /* 9 */
};

#define SEG_CHAR_D    0x3DU
#define SEG_CHAR_O    0x1DU
#define SEG_CHAR_N    0x15U
#define SEG_CHAR_E    0x4FU
#define SEG_BLANK     0x00U
#define SEG_DP        0x80U

/*
 * The schematic ties SEG_DP_OUT to DIG1_OUT only, so the colon/decimal-point
 * can only appear on the second digit from the left. In the firmware's digit
 * mapping, digit indexes are written left-to-right as 3, 2, 1, 0.
 */
#define DISPLAY_COLON_DIGIT_INDEX 1U

static const struct device *spi_bus;
static struct spi_config spi_cfg;
static struct spi_cs_control spi_cs;

static bool display_initialized;
static uint8_t digit_cache[8];
static bool digit_cache_valid[8];

static int max7221_write(uint8_t reg, uint8_t data)
{
    uint8_t tx_buf[2] = { reg, data };
    struct spi_buf spi_tx = {
        .buf = tx_buf,
        .len = sizeof(tx_buf),
    };
    struct spi_buf_set tx = {
        .buffers = &spi_tx,
        .count = 1,
    };

    return spi_write(spi_bus, &spi_cfg, &tx);
}

static void invalidate_cache(void)
{
    memset(digit_cache_valid, 0, sizeof(digit_cache_valid));
}

static int max7221_write_digit(uint8_t digit_index, uint8_t data)
{
    int err;

    if (digit_index >= 8U) {
        return -EINVAL;
    }

    if (digit_cache_valid[digit_index] && digit_cache[digit_index] == data) {
        return 0;
    }

    err = max7221_write(MAX7221_REG_DIGIT0 + digit_index, data);
    if (err == 0) {
        digit_cache[digit_index] = data;
        digit_cache_valid[digit_index] = true;
    }

    return err;
}

static int max7221_write_digits(const uint8_t digits[8])
{
    int err;

    /*
     * Fix 4: Insert a guaranteed dark period before changing any digit.
     * Write SEG_BLANK to every digit whose value is about to change, then
     * write the real values in a second pass.  This eliminates the brief
     * moment where the wrong segment pattern is driven into a newly-selected
     * digit (the root cause of ghosting).
     */
    for (uint8_t i = 0; i < 8U; ++i) {
        if (!digit_cache_valid[i] || digit_cache[i] != digits[i]) {
            err = max7221_write(MAX7221_REG_DIGIT0 + i, SEG_BLANK);
            if (err) {
                return err;
            }
            /* Invalidate so the second pass always writes the real value. */
            digit_cache_valid[i] = false;
        }
    }

    for (uint8_t i = 0; i < 8U; ++i) {
        err = max7221_write_digit(i, digits[i]);
        if (err) {
            return err;
        }
    }

    return 0;
}

int display_init(void)
{
    int err;
    struct gpio_dt_spec cs_gpio;

    display_initialized = false;
    invalidate_cache();
    memset(digit_cache, 0, sizeof(digit_cache));

    spi_bus = DEVICE_DT_GET(DT_NODELABEL(spi1));
    if (!device_is_ready(spi_bus)) {
        LOG_ERR("SPI bus device not ready");
        return -ENODEV;
    }

    cs_gpio = (struct gpio_dt_spec)GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi1), cs_gpios, 0);
    if (!gpio_is_ready_dt(&cs_gpio)) {
        LOG_ERR("SPI CS GPIO device not ready");
        return -ENODEV;
    }

    spi_cs.gpio = cs_gpio;
    spi_cs.delay = 0U;

    spi_cfg.frequency = MAX7221_SPI_FREQUENCY_HZ;
    spi_cfg.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB;
    spi_cfg.slave = 0U;
    spi_cfg.cs = spi_cs;

    /* Configure while in shutdown, then enable at the end. */
    err = max7221_write(MAX7221_REG_SHUTDOWN, 0x00);
    if (err) {
        LOG_ERR("Failed to enter shutdown during init (err %d)", err);
        return err;
    }

    err = max7221_write(MAX7221_REG_DISPLAY_TEST, 0x00);
    if (err) {
        LOG_ERR("Failed to disable display test (err %d)", err);
        return err;
    }

    err = max7221_write(MAX7221_REG_DECODE_MODE, 0x00);
    if (err) {
        LOG_ERR("Failed to set decode mode (err %d)", err);
        return err;
    }

    err = max7221_write(MAX7221_REG_SCAN_LIMIT, MAX7221_SCAN_LIMIT_4_DIGITS);
    if (err) {
        LOG_ERR("Failed to set scan limit (err %d)", err);
        return err;
    }

    err = max7221_write(MAX7221_REG_INTENSITY, MAX7221_DEFAULT_INTENSITY);
    if (err) {
        LOG_ERR("Failed to set intensity (err %d)", err);
        return err;
    }

    for (uint8_t i = 0; i < 8U; ++i) {
        err = max7221_write_digit(i, SEG_BLANK);
        if (err) {
            LOG_ERR("Failed to clear digit %u (err %d)", i, err);
            invalidate_cache();
            return err;
        }
    }

    err = max7221_write(MAX7221_REG_SHUTDOWN, 0x01);
    if (err) {
        LOG_ERR("Failed to exit shutdown (err %d)", err);
        invalidate_cache();
        return err;
    }

    display_initialized = true;
    LOG_INF("MAX7221 display initialized");
    return 0;
}

void display_time(uint32_t time_ms, bool show_minutes)
{
    uint8_t digits[8] = { SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK,
                          SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK };

    if (!display_initialized) {
        return;
    }

    if (show_minutes) {
        uint32_t total_seconds = time_ms / 1000U;
        uint32_t minutes = total_seconds / 60U;
        uint8_t seconds = (uint8_t)(total_seconds % 60U);

        if (minutes > 99U) {
            minutes = 99U;
        }

        digits[3] = SEGMENT_FONT[(minutes / 10U) % 10U];
        digits[2] = SEGMENT_FONT[minutes % 10U];
        digits[1] = SEGMENT_FONT[(seconds / 10U) % 10U] | SEG_DP;
        digits[0] = SEGMENT_FONT[seconds % 10U];
    } else {
        uint32_t total_centiseconds = time_ms / 10U;
        uint8_t seconds = (uint8_t)((total_centiseconds / 100U) % 100U);
        uint8_t centiseconds = (uint8_t)(total_centiseconds % 100U);

        digits[3] = SEGMENT_FONT[(seconds / 10U) % 10U];
        digits[2] = SEGMENT_FONT[seconds % 10U];
        digits[1] = SEGMENT_FONT[(centiseconds / 10U) % 10U] | SEG_DP;
        digits[0] = SEGMENT_FONT[centiseconds % 10U];
    }

    (void)max7221_write_digits(digits);
}

void display_clear(void)
{
    uint8_t digits[8] = { SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK,
                          SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK };

    if (!display_initialized) {
        return;
    }

    (void)max7221_write_digits(digits);
}

void display_done(void)
{
    uint8_t digits[8] = { SEG_CHAR_E, SEG_CHAR_N, SEG_CHAR_O, SEG_CHAR_D,
                          SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK };

    if (!display_initialized) {
        return;
    }

    (void)max7221_write_digits(digits);
}
