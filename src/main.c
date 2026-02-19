/*
 * Race Timer Firmware - Main Entry Point
 *
 * Digital Electronic Stopwatch firmware for nRF52840 (MDBT50Q-P1M).
 * Communicates with iOS app via BLE to manage race timing.
 *
 * Hardware:
 *   - MCU: nRF52840 (MDBT50Q-P1MV2 module)
 *   - 4 race buttons + 1 auxiliary (JST connectors J2-J6)
 *   - 5 button LEDs (12V, driven via NPN transistors)
 *   - 1 status LED (Blue, P0.06)
 *   - 4-digit seven-segment display (MAX7221 via SPI)
 *   - Power: 12V DC → 5V (LD1117) → 3.3V (TLV75733)
 *
 * Features:
 *   - BLE peripheral with custom Race Timer GATT service
 *   - Mode 1: 4 independent contestants (first press = winner)
 *   - Mode 2: 1 contestant, 4 sequential segments
 *   - Millisecond-resolution timing (mainboard is source of truth)
 *   - Real-time display update on seven-segment display
 *   - Button LED control for visual feedback
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "race_manager.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    int err;

    /* Brief delay to allow USB CDC ACM to enumerate (Xiao BLE) */
    k_msleep(1000);

    LOG_INF("======================================");
    LOG_INF("  Race Timer Firmware v1.0.0");
    LOG_INF("  nRF52840 / MDBT50Q-P1M");
    LOG_INF("======================================");

    printf("\n");
    printf("======================================\n");
    printf("  Race Timer Firmware v1.0.0\n");
    printf("  nRF52840 — UART Console Active\n");
    printf("======================================\n");

    /* Initialize the race manager (initializes all subsystems) */
    err = race_manager_init();
    if (err) {
        LOG_ERR("Race manager init failed (err %d)", err);
        printf("ERROR: Race manager init failed (err %d)\n", err);
        return err;
    }

    LOG_INF("System ready. Waiting for BLE connection...");
    printf("System ready. Waiting for BLE connection...\n");

    return 0;
}

