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

#include <string.h>
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

/*
 * Raw segment font (no-decode mode).
 * Bit mapping: DP G F E D C B A  (bit 7 down to bit 0)
 *
 *        A
 *       ---
 *   F |     | B
 *       -G-
 *   E |     | C
 *       ---
 *        D    .DP
 */
static const uint8_t SEGMENT_FONT[] = {
    0x7E,  /* 0: A B C D E F     */
    0x30,  /* 1: B C             */
    0x6D,  /* 2: A B D E G       */
    0x79,  /* 3: A B C D G       */
    0x33,  /* 4: B C F G         */
    0x5B,  /* 5: A C D F G       */
    0x5F,  /* 6: A C D E F G     */
    0x70,  /* 7: A B C           */
    0x7F,  /* 8: A B C D E F G   */
    0x7B,  /* 9: A B C D F G     */
};

#define SEG_E      0x4F  /* A D E F G           */
#define SEG_BLANK  0x00  /* all segments off    */
#define SEG_DP     0x80  /* decimal point bit   */

/* SPI bus device (no child node - raw SPI access to MAX7221) */
static const struct device *spi_bus;
static struct spi_config spi_cfg;
static struct spi_cs_control spi_cs;

static bool display_initialized;

/* Cache of last-written digit values to avoid redundant SPI writes */
static uint8_t digit_cache[8];
static bool digit_cache_valid[8];

/**
 * @brief Write a register value to the MAX7221.
 */
static int max7221_write(uint8_t reg, uint8_t data);

/**
 * @brief Write a digit only if its value has changed since the last write.
 */
static void max7221_write_digit(uint8_t digit_index, uint8_t data)
{
    if (digit_index < 8 && digit_cache_valid[digit_index] &&
        digit_cache[digit_index] == data) {
        return;
    }
    max7221_write(MAX7221_REG_DIGIT0 + digit_index, data);
    if (digit_index < 8) {
        digit_cache[digit_index] = data;
        digit_cache_valid[digit_index] = true;
    }
}

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

    /* Configure SPI: Mode 0, 8-bit, MSB first. 1 MHz to reduce edges/noise (helps display ghosting). */
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
    err = max7221_write(MAX7221_REG_SCAN_LIMIT, 0x04);
    if (err) {
        LOG_ERR("Failed to set scan limit (err %d)", err);
        return err;
    }

    /* No decode — raw segment mode for all digits */
    err = max7221_write(MAX7221_REG_DECODE_MODE, 0x00);
    if (err) {
        LOG_ERR("Failed to set decode mode (err %d)", err);
        return err;
    }

    /* Set medium brightness */
    err = max7221_write(MAX7221_REG_INTENSITY, 0x00);
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

    /* Clear all digits (0x00 = all segments off in raw mode) */
    for (int i = 0; i < 8; i++) {
        max7221_write(MAX7221_REG_DIGIT0 + i, SEG_BLANK);
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

        max7221_write(MAX7221_REG_DIGIT3, SEGMENT_FONT[minutes / 10] | SEG_DP);
        max7221_write(MAX7221_REG_DIGIT2, SEGMENT_FONT[minutes % 10] | SEG_DP);
        max7221_write(MAX7221_REG_DIGIT1, SEGMENT_FONT[seconds / 10] | SEG_DP);
        max7221_write(MAX7221_REG_DIGIT0, SEGMENT_FONT[seconds % 10] | SEG_DP);

        for (int i = 4; i < 8; i++) {

            max7221_write(MAX7221_REG_DIGIT0 + i, SEG_BLANK);
        }




    } else {
        /* SS.mm format (seconds and hundredths) */
        uint32_t total_centiseconds = time_ms / 10;
        uint8_t seconds = (total_centiseconds / 100) % 100;
        uint8_t centiseconds = total_centiseconds % 100;

        max7221_write_digit(3, SEGMENT_FONT[seconds / 10]| SEG_DP);
        max7221_write_digit(2, SEGMENT_FONT[seconds % 10] | SEG_DP);
        max7221_write_digit(1, SEGMENT_FONT[centiseconds / 10]| SEG_DP);
        max7221_write_digit(0, SEGMENT_FONT[centiseconds % 10]| SEG_DP);
    }
}

void display_clear(void)
{
    if (!display_initialized) {
        return;
    }

    for (int i = 0; i < DISPLAY_NUM_DIGITS; i++) {
        max7221_write(MAX7221_REG_DIGIT0 + i, SEG_BLANK);
    }
    memset(digit_cache_valid, 0, sizeof(digit_cache_valid));
}

void display_done(void)
{
    if (!display_initialized) {
        return;
    }

    /* "donE" — raw mode lets us define any character:
     *   d = segments B C D E G  = 0x3D
     *   o = segments C D E G    = 0x1D
     *   n = segments C E G      = 0x15
     *   E = segments A D E F G  = SEG_E
     */
    max7221_write(MAX7221_REG_DIGIT0, 0x3D);    /* d */
    max7221_write(MAX7221_REG_DIGIT1, 0x1D);    /* o */
    max7221_write(MAX7221_REG_DIGIT2, 0x15);    /* n */
    max7221_write(MAX7221_REG_DIGIT3, SEG_E);   /* E */
    memset(digit_cache_valid, 0, sizeof(digit_cache_valid));
}

