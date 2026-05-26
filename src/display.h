/*
 * Seven-Segment Display Driver (TLC5926 daisy-chain, board V2.0.0)
 *
 * Two TLC5926 16-bit constant-current sink drivers — one per segment board
 * (Segment A on the left, Segment B on the right) — chained as a single
 * 32-bit shift register:
 *
 *   nRF SDI → Seg B SDI ─┐
 *                        └→ Seg B SDO → Seg A SDI ─┐
 *                                                   └→ Seg A SDO → nRF (loop-back, unused)
 *
 * Shared signals on the chain (P0.x pins on the nRF52840 module U3):
 *   SCLK → P0.19 (SPI1 SCK)        — shift clock for both chips
 *   SDI  → P0.21 (SPI1 MOSI)       — serial data into chain
 *   LE   → P0.24 (GPIO)            — pulse high to latch shifted data into outputs
 *   OE   → P0.25 (PWM1 CH0)        — active-LOW output enable / brightness PWM
 *
 * Per-chip output mapping (from Seg A schematic):
 *   OUT0..6  = right-digit segments A..G
 *   OUT7..13 = left-digit  segments A..G
 *   OUT14    = (Seg A) unused / (Seg B) SEG_COLON
 *   OUT15    = unused on both
 *
 * Chain bit layout in the 32-bit word (sent MSB-first; the first 16 bits
 * end up in Seg A because it is further down the chain):
 *
 *   bits 31..24 → Seg A high byte  (bit 31 = OUT15, bit 24 = OUT8)
 *   bits 23..16 → Seg A low byte   (bit 23 = OUT7,  bit 16 = OUT0)
 *   bits 15..8  → Seg B high byte  (bit 15 = OUT15, bit 14 = COLON, bit 8 = OUT8)
 *   bits  7..0  → Seg B low byte   (bit  7 = OUT7,  bit  0 = OUT0)
 *
 * Logical clock digit positions (left → right): digit3 digit2 [:] digit1 digit0
 *   digit3, digit2 live on Seg A (left board)
 *   digit1, digit0 live on Seg B (right board)
 */

#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <zephyr/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_NUM_DIGITS 4

/**
 * @brief Initialize the TLC5926 daisy-chain display driver.
 * Clears the display and turns brightness on at full.
 * @return 0 on success, negative errno on failure.
 */
int display_init(void);

/**
 * @brief Display a time value in MM:SS format (colon always on).
 *
 * Values above 99:59 are clamped to 99:59.
 *
 * @param time_ms Time in milliseconds.
 */
void display_time(uint32_t time_ms);

/**
 * @brief Clear the display (all segments and colon off).
 */
void display_clear(void);

/**
 * @brief Display "donE" finish indicator.
 */
void display_done(void);

/**
 * @brief Set display brightness via PWM on the TLC5926 OE pin.
 *
 * @param percent 0..100 (0 = off, 100 = full brightness). Values above 100 are
 *        clamped.
 * @return 0 on success, negative errno on failure.
 */
int display_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H_ */
