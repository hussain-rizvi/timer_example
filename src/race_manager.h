/*
 * Race Manager
 *
 * Implements the race state machine and coordinates all subsystems:
 * BLE, buttons, LEDs, display, and timer.
 *
 * State Machine:
 *   DISCONNECTED → IDLE → CONFIGURED → RUNNING → FINISHED
 *                                          ↓
 *                                        ERROR
 *
 * Race Modes:
 *   Mode 1: 4 independent contestants (first button press = winner)
 *   Mode 2: 1 contestant, 4 segments (sequential button presses)
 */

#ifndef RACE_MANAGER_H_
#define RACE_MANAGER_H_

#include <zephyr/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Race States ── */
typedef enum {
    RACE_STATE_DISCONNECTED = 0,
    RACE_STATE_IDLE,         /* BLE connected, no race configured */
    RACE_STATE_CONFIGURED,   /* Mode set, ready to start race */
    RACE_STATE_RUNNING,      /* Race in progress, timer counting */
    RACE_STATE_FINISHED,     /* Race complete, results available */
    RACE_STATE_ERROR,        /* Error state (BLE lost during race, etc.) */
} race_state_t;

/* ── Race Result for one button/lane/segment ── */
struct race_result {
    uint8_t  button_index;   /* Which button (1-4) */
    uint32_t elapsed_ms;     /* Time when button was pressed */
    bool     recorded;       /* Whether this result has been captured */
};

#define MAX_RESULTS 4

/* ── Race Data ── */
struct race_data {
    race_state_t state;
    uint8_t      mode;                      /* RACE_MODE_4_CONTESTANTS or RACE_MODE_1_CONTESTANT */
    uint32_t     race_id;                   /* Unique race identifier (timestamp) */
    struct race_result results[MAX_RESULTS]; /* Results for buttons 1-4 */
    uint8_t      results_count;             /* Number of results recorded */
    uint8_t      winner_button;             /* Winner button index (Mode 1), 0 if none */
    uint32_t     winner_time_ms;            /* Winner's elapsed time */
    bool         race_complete;             /* All expected results received */
};

/**
 * @brief Initialize the race manager and all subsystems.
 * @return 0 on success, negative errno on failure.
 */
int race_manager_init(void);

/**
 * @brief Get the current race state.
 * @return Current race_state_t.
 */
race_state_t race_manager_get_state(void);

/**
 * @brief Get the current race data (read-only).
 * @return Pointer to current race data.
 */
const struct race_data *race_manager_get_data(void);

/**
 * @brief Process the race manager (call periodically from main loop).
 *        Updates display, handles timeouts, etc.
 */
void race_manager_process(void);

#ifdef __cplusplus
}
#endif

#endif /* RACE_MANAGER_H_ */

