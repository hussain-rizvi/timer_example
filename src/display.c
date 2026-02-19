/*
 * MAX7221 Seven-Segment Display Driver Implementation
 *
 * The MAX7221 is controlled via SPI. It has a 16-bit shift register:
 *   [15:12] = address (register select)
 *   [11:8]  = don't care
 *   [7:0]   = data
 *
 * Actually the format is:
 *   Byte 1 (MSB first): [D15..D8] = {X, X, X, X, ADDR[3:0]}
 *   Byte 2:             [D7..D0]  = DATA[7:0]
 *
 * MAX7221 Registers:
 *   0x01-0x08: Digit 0-7 data
 *   0x09: Decode Mode
 *   0x0A: Intensity
 *   0x0B: Scan Limit
 *   0x0C: Shutdown
 *   0x0F: Display Test
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "display.h"

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/* MAX7221 Register Addresses */
#define MAX7221_REG_NOOP        0x00
#define MAX7221_REG_DIGIT0      0x01
#define MAX7221_REG_DIGIT1      0x02
#define MAX7221_REG_DIGIT2      0x03
#define MAX7221_REG_DIGIT3      0x04
#define MAX7221_REG_DIGIT4      0x05
#define MAX7221_REG_DIGIT5      0x06
#define MAX7221_REG_DIGIT6      0x07
#define MAX7221_REG_DIGIT7      0x08
#define MAX7221_REG_DECODE_MODE 0x09
#define MAX7221_REG_INTENSITY   0x0A
#define MAX7221_REG_SCAN_LIMIT  0x0B
#define MAX7221_REG_SHUTDOWN    0x0C
#define MAX7221_REG_DISPLAY_TEST 0x0F

/* BCD decode values for MAX7221 */
#define CHAR_DASH  0x0A  /* Displays '-' in BCD mode */
#define CHAR_E     0x0B  /* Displays 'E' in BCD mode */
#define CHAR_H     0x0C  /* Displays 'H' in BCD mode */
#define CHAR_L     0x0D  /* Displays 'L' in BCD mode */
#define CHAR_P     0x0E  /* Displays 'P' in BCD mode */
#define CHAR_BLANK 0x0F  /* Blank in BCD mode */
#define CHAR_DP    0x80  /* Decimal point bit */

/* SPI bus device (no child node - raw SPI access to MAX7221) */
static const struct device *spi_bus;
static struct spi_config spi_cfg;
static struct spi_cs_control spi_cs;

static bool display_initialized;

/**
 * @brief Write a register value to the MAX7221.
 */
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

int display_init(void)
{
    int err;

    /* Get the SPI bus device */
    spi_bus = DEVICE_DT_GET(DT_NODELABEL(spi1));
    if (!device_is_ready(spi_bus)) {
        LOG_ERR("SPI bus device not ready");
        return -ENODEV;
    }

    /* Configure CS pin */
    spi_cs.gpio = (struct gpio_dt_spec)GPIO_DT_SPEC_GET_BY_IDX(
        DT_NODELABEL(spi1), cs_gpios, 0);
    spi_cs.delay = 0;

    /* Configure SPI settings: Mode 0, 8-bit, MSB first, 1MHz */
    spi_cfg.frequency = 1000000U;
    spi_cfg.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB;
    spi_cfg.slave = 0;
    spi_cfg.cs = spi_cs;

    /* Exit shutdown mode */
    err = max7221_write(MAX7221_REG_SHUTDOWN, 0x01);
    if (err) {
        LOG_ERR("Failed to exit shutdown (err %d)", err);
        return err;
    }

    /* Set scan limit to 4 digits (0-3) */
    err = max7221_write(MAX7221_REG_SCAN_LIMIT, 0x03);
    if (err) {
        LOG_ERR("Failed to set scan limit (err %d)", err);
        return err;
    }

    /* Enable BCD decode for digits 0-3 */
    err = max7221_write(MAX7221_REG_DECODE_MODE, 0x0F);
    if (err) {
        LOG_ERR("Failed to set decode mode (err %d)", err);
        return err;
    }

    /* Set medium brightness */
    err = max7221_write(MAX7221_REG_INTENSITY, 0x08);
    if (err) {
        LOG_ERR("Failed to set intensity (err %d)", err);
        return err;
    }

    /* Disable display test */
    err = max7221_write(MAX7221_REG_DISPLAY_TEST, 0x00);
    if (err) {
        LOG_ERR("Failed to disable display test (err %d)", err);
        return err;
    }

    /* Clear all digits */
    for (int i = 0; i < DISPLAY_NUM_DIGITS; i++) {
        max7221_write(MAX7221_REG_DIGIT0 + i, CHAR_BLANK);
    }
    /* Also blank unused digits 4-7 */
    for (int i = 4; i < 8; i++) {
        max7221_write(MAX7221_REG_DIGIT0 + i, CHAR_BLANK);
    }

    display_initialized = true;
    LOG_INF("MAX7221 display initialized");
    return 0;
}

void display_time(uint32_t time_ms, bool show_minutes)
{
    if (!display_initialized) {
        return;
    }

    if (show_minutes) {
        /* MM:SS format */
        uint32_t total_seconds = time_ms / 1000;
        uint32_t minutes = total_seconds / 60;
        uint8_t seconds = total_seconds % 60;

        if (minutes > 99) {
            minutes = 99;
        }

        /* Digit 0 (leftmost): tens of minutes */
        max7221_write(MAX7221_REG_DIGIT0, (minutes / 10) & 0x0F);
        /* Digit 1: ones of minutes + decimal point (acts as colon) */
        max7221_write(MAX7221_REG_DIGIT1, ((minutes % 10) & 0x0F) | CHAR_DP);
        /* Digit 2: tens of seconds */
        max7221_write(MAX7221_REG_DIGIT2, (seconds / 10) & 0x0F);
        /* Digit 3 (rightmost): ones of seconds */
        max7221_write(MAX7221_REG_DIGIT3, (seconds % 10) & 0x0F);
    } else {
        /* SS.mm format (seconds and hundredths) */
        uint32_t total_centiseconds = time_ms / 10;
        uint8_t seconds = (total_centiseconds / 100) % 100;
        uint8_t centiseconds = total_centiseconds % 100;

        /* Digit 0: tens of seconds */
        max7221_write(MAX7221_REG_DIGIT0, (seconds / 10) & 0x0F);
        /* Digit 1: ones of seconds + decimal point */
        max7221_write(MAX7221_REG_DIGIT1, ((seconds % 10) & 0x0F) | CHAR_DP);
        /* Digit 2: tens of centiseconds */
        max7221_write(MAX7221_REG_DIGIT2, (centiseconds / 10) & 0x0F);
        /* Digit 3: ones of centiseconds */
        max7221_write(MAX7221_REG_DIGIT3, (centiseconds % 10) & 0x0F);
    }
}

void display_number(uint16_t value, int8_t decimal_pos)
{
    if (!display_initialized) {
        return;
    }

    if (value > 9999) {
        value = 9999;
    }

    uint8_t digits[4];
    digits[0] = (value / 1000) % 10;
    digits[1] = (value / 100) % 10;
    digits[2] = (value / 10) % 10;
    digits[3] = value % 10;

    for (int i = 0; i < DISPLAY_NUM_DIGITS; i++) {
        uint8_t data = digits[i] & 0x0F;
        if (decimal_pos == i) {
            data |= CHAR_DP;
        }
        max7221_write(MAX7221_REG_DIGIT0 + i, data);
    }
}

void display_raw(uint8_t digit, uint8_t segments)
{
    if (!display_initialized || digit >= DISPLAY_NUM_DIGITS) {
        return;
    }

    /* For raw segments, we need to disable BCD decode for this digit
     * This is a simplified version - for mixed raw/BCD, decode mode
     * register would need per-digit control. */
    max7221_write(MAX7221_REG_DIGIT0 + digit, segments);
}

void display_clear(void)
{
    if (!display_initialized) {
        return;
    }

    for (int i = 0; i < DISPLAY_NUM_DIGITS; i++) {
        max7221_write(MAX7221_REG_DIGIT0 + i, CHAR_BLANK);
    }
}

void display_set_brightness(uint8_t intensity)
{
    if (!display_initialized) {
        return;
    }

    if (intensity > 15) {
        intensity = 15;
    }

    max7221_write(MAX7221_REG_INTENSITY, intensity);
}

void display_power(bool on)
{
    if (!display_initialized) {
        return;
    }

    max7221_write(MAX7221_REG_SHUTDOWN, on ? 0x01 : 0x00);
}

void display_dashes(void)
{
    if (!display_initialized) {
        return;
    }

    for (int i = 0; i < DISPLAY_NUM_DIGITS; i++) {
        max7221_write(MAX7221_REG_DIGIT0 + i, CHAR_DASH);
    }
}

void display_done(void)
{
    if (!display_initialized) {
        return;
    }

    /* Display "donE" using BCD special chars where possible
     * d=0x0D? Actually BCD doesn't have 'd'. We'll display "----" instead
     * or use a numeric indicator. For simplicity, display "0000" meaning done,
     * or we can show the winning time instead. */

    /* Show "End" approximation: E n d blank -> Using available chars: */
    /* MAX7221 BCD: 0-9, -, E, H, L, P, blank */
    /* Best we can do: "E--E" or show final time */
    max7221_write(MAX7221_REG_DIGIT0, CHAR_BLANK);
    max7221_write(MAX7221_REG_DIGIT1, CHAR_E);
    max7221_write(MAX7221_REG_DIGIT2, CHAR_DASH);
    max7221_write(MAX7221_REG_DIGIT3, CHAR_DASH);
}

