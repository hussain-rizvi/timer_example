/*
 * Seven-Segment Display Driver (MAX7221)
 *
 * Drives a 4-digit seven-segment display via MAX7221 LED driver
 * connected over SPI through an LSF0204 level shifter (3.3V → 5V).
 *
 * SPI Pin Mapping (from schematic):
 *   MOSI → P0.21 → LSF0204 A3 → B3 → MAX7221 DIN (pin 1)
 *   SCK  → P0.19 → LSF0204 A4 → B4 → MAX7221 CLK (pin 13)
 *   CS   → P0.25 → LSF0204 A1 → B1 → MAX7221 CS  (pin 12)
 *
 * MAX7221 drives DIG0-DIG3 (4 digits) through anode/cathode driver circuits:
 *   - Anode drivers: AO3401A P-MOSFETs for segments (12V rail)
 *   - Cathode drivers: AO3400A N-MOSFETs for digit selection
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
 * @brief Initialize the MAX7221 display driver over SPI.
 * @return 0 on success, negative errno on failure.
 */
int display_init(void);

/**
 * @brief Display a time value in MM:SS or SS.mm format.
 *
 * @param time_ms Time in milliseconds.
 * @param show_minutes If true, display MM:SS format. If false, SS.mm (hundredths).
 */
void display_time(uint32_t time_ms, bool show_minutes);

/**
 * @brief Display a raw 4-digit number.
 *
 * @param value Number to display (0-9999).
 * @param decimal_pos Position of decimal point (0-3, or -1 for none).
 */
void display_number(uint16_t value, int8_t decimal_pos);

/**
 * @brief Display raw segment data on a specific digit.
 *
 * @param digit Digit position (0-3, left to right).
 * @param segments Segment bitmask (bit 0=A, ..., bit 6=G, bit 7=DP).
 */
void display_raw(uint8_t digit, uint8_t segments);

/**
 * @brief Clear the display (all segments off).
 */
void display_clear(void);

/**
 * @brief Set display brightness.
 *
 * @param intensity Brightness level (0-15).
 */
void display_set_brightness(uint8_t intensity);

/**
 * @brief Turn display on or off.
 *
 * @param on true = display on, false = shutdown mode.
 */
void display_power(bool on);

/**
 * @brief Display dashes (----) to indicate ready/idle state.
 */
void display_dashes(void);

/**
 * @brief Display "donE" or similar finish indicator.
 */
void display_done(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H_ */

