/*
 * Race Manager Implementation
 *
 * Central coordination of all race timer subsystems.
 * Handles the race state machine, BLE commands, button events,
 * display updates, and LED indicators.
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "race_manager.h"
#include "ble_race_service.h"
#include "buttons.h"
#include "leds.h"
#include "display.h"
#include "race_timer.h"

LOG_MODULE_REGISTER(race_mgr, LOG_LEVEL_INF);

/* ── Race Data ── */
static struct race_data race;

/* ── Display Update Timer ── */
static struct k_work_delayable display_update_work;
#define DISPLAY_UPDATE_INTERVAL_MS 50  /* Update display every 50ms during race */

/* ── Button 5 (auxiliary) ── */
#define BUTTON_AUX  5  /* Button 5 = manual start/stop */

/* ── Forward Declarations ── */
static void on_ble_command(const struct race_cmd_packet *cmd);
static void on_ble_connection_change(bool connected);
static void on_button_press(uint8_t button_index, uint32_t timestamp_ms);
static void set_state(race_state_t new_state);
static void handle_start_race(void);
static void handle_new_race(void);
static void handle_stop_race(void);
static void handle_button5_press(void);
static void handle_button_during_race(uint8_t button_index);
static void send_event(uint8_t event_type, uint8_t button_index, uint32_t elapsed_ms);
static void display_update_handler(struct k_work *work);
static void update_leds_for_state(void);

/* ── BLE Callbacks ── */
static const struct ble_race_service_cb ble_callbacks = {
    .on_command = on_ble_command,
    .on_connection_change = on_ble_connection_change,
};

/* ── Initialization ── */

int race_manager_init(void)
{
    int err;

    /* Initialize race data */
    memset(&race, 0, sizeof(race));
    race.state = RACE_STATE_DISCONNECTED;

    /* Initialize subsystems */
    err = leds_init();
    if (err) {
        LOG_ERR("LED init failed (err %d)", err);
        return err;
    }

    err = buttons_init(on_button_press);
    if (err) {
        LOG_ERR("Button init failed (err %d)", err);
        return err;
    }

    err = display_init();
    if (err) {
        LOG_ERR("Display init failed (err %d)", err);
        /* Non-fatal: continue without display */
        LOG_WRN("Continuing without display");
    }

    err = race_timer_init();
    if (err) {
        LOG_ERR("Timer init failed (err %d)", err);
        return err;
    }

    /* Initialize BLE */
    err = ble_race_service_init(&ble_callbacks);
    if (err) {
        LOG_ERR("BLE init failed (err %d)", err);
        return err;
    }

    /* Start advertising */
    err = ble_race_service_start_advertising();
    if (err) {
        LOG_ERR("Advertising start failed (err %d)", err);
        return err;
    }

    /* Initialize display update work */
    k_work_init_delayable(&display_update_work, display_update_handler);

    /* Enable button 5 (start/stop) at all times.
     * Buttons 1-4 are enabled/disabled per race state by
     * handle_start_race() and handle_new_race(). Button 5 uses
     * the same ISR but is always active so it can start races. */
    buttons_enable();

    /* Show idle state */
    display_dashes();
    leds_set_status(false);
    leds_status_blink(500, 500, 0);  /* Slow blink while disconnected */

    LOG_INF("Race manager initialized");
    printf("Race manager initialized OK\n");
    printf("  Buttons: 4 race + 1 aux (start/stop)\n");
    printf("  Modes: 1=4-contestant, 2=1-contestant\n");
    printf("  Press Button 5 to start standalone race\n");
    return 0;
}

/* ── State Machine ── */

static const char *state_name(race_state_t s)
{
    switch (s) {
    case RACE_STATE_DISCONNECTED: return "DISCONNECTED";
    case RACE_STATE_IDLE:         return "IDLE";
    case RACE_STATE_CONFIGURED:   return "CONFIGURED";
    case RACE_STATE_RUNNING:      return "RUNNING";
    case RACE_STATE_FINISHED:     return "FINISHED";
    case RACE_STATE_ERROR:        return "ERROR";
    default:                      return "UNKNOWN";
    }
}

static void set_state(race_state_t new_state)
{
    if (race.state == new_state) {
        return;
    }

    LOG_INF("State: %d → %d", race.state, new_state);
    printf("STATE: %s -> %s\n", state_name(race.state), state_name(new_state));
    race.state = new_state;

    /* Update BLE status characteristic */
    ble_race_service_set_status((uint8_t)new_state);

    /* Update visual indicators for new state */
    update_leds_for_state();
}

static void update_leds_for_state(void)
{
    switch (race.state) {
    case RACE_STATE_DISCONNECTED:
        leds_all_off();
        leds_status_blink(200, 800, 0);  /* Fast-blink status while disconnected */
        display_dashes();
        break;

    case RACE_STATE_IDLE:
        leds_all_off();
        leds_set_status(true);  /* Solid status = connected */
        display_dashes();
        break;

    case RACE_STATE_CONFIGURED:
        /* Light up button LEDs for active lanes */
        leds_all_off();
        if (race.mode == RACE_MODE_4_CONTESTANTS) {
            for (int i = 1; i <= NUM_RACE_BUTTONS; i++) {
                leds_set_button(i, true);
            }
        } else {
            leds_set_button(1, true);  /* Only lane 1 active in single mode */
        }
        leds_set_status(true);
        display_clear();
        display_time(0, true);  /* Show 00:00 */
        break;

    case RACE_STATE_RUNNING:
        leds_set_status(true);
        /* Start display update timer */
        k_work_reschedule(&display_update_work, K_NO_WAIT);
        break;

    case RACE_STATE_FINISHED:
        /* Turn off unfinished lane LEDs, keep winner LED on */
        leds_all_off();
        if (race.winner_button > 0 && race.winner_button <= NUM_BUTTON_LEDS) {
            leds_set_button(race.winner_button, true);
        }
        leds_set_status(true);
        /* Stop display updates - show final time */
        k_work_cancel_delayable(&display_update_work);
        if (race.winner_time_ms > 0) {
            display_time(race.winner_time_ms, true);  /* MM:SS */
        } else {
            display_done();
        }
        break;

    case RACE_STATE_ERROR:
        leds_all_off();
        leds_status_blink(100, 100, 10);  /* Rapid blink on error */
        display_dashes();
        break;
    }
}

/* ── BLE Command Handler ── */

static void on_ble_command(const struct race_cmd_packet *cmd)
{
    LOG_INF("Command received: type=0x%02X, mode=0x%02X", cmd->cmd_type, cmd->mode);
    printf("CMD: type=0x%02X, mode=0x%02X\n", cmd->cmd_type, cmd->mode);

    switch (cmd->cmd_type) {
    case CMD_START_RACE:
        handle_start_race();
        break;

    case CMD_NEW_RACE:
    case CMD_RESET:
        handle_new_race();
        break;

    case CMD_SET_MODE:
        if (race.state == RACE_STATE_IDLE || race.state == RACE_STATE_CONFIGURED) {
            race.mode = cmd->mode;
            set_state(RACE_STATE_CONFIGURED);
            LOG_INF("Mode set to %d", race.mode);
            printf("MODE: Set to %d (%s)\n", race.mode,
                   race.mode == RACE_MODE_4_CONTESTANTS ? "4-contestant" : "1-contestant");
        } else {
            LOG_WRN("Cannot set mode in state %d", race.state);
            printf("WARNING: Cannot set mode in state %s\n", state_name(race.state));
        }
        break;

    case CMD_PING:
        printf("PING: Received, sending PONG\n");
        send_event(EVT_PONG, 0, 0);
        break;

    case CMD_GET_STATUS:
        printf("STATUS: Requested, elapsed=%u ms\n", race_timer_get_ms());
        send_event(EVT_STATUS, 0, race_timer_get_ms());
        break;

    default:
        LOG_WRN("Unknown command: 0x%02X", cmd->cmd_type);
        printf("WARNING: Unknown command 0x%02X\n", cmd->cmd_type);
        send_event(EVT_ERROR, 0, 0);
        break;
    }
}

/* ── Connection State Handler ── */

static void on_ble_connection_change(bool connected)
{
    if (connected) {
        LOG_INF("BLE connected");
        printf("BLE: Connected\n");
        if (race.state == RACE_STATE_DISCONNECTED) {
            set_state(RACE_STATE_IDLE);
        } else if (race.state == RACE_STATE_ERROR) {
            /* Reconnected after error during race */
            if (race_timer_is_running()) {
                set_state(RACE_STATE_RUNNING);
                send_event(EVT_STATUS, 0, race_timer_get_ms());
            } else {
                set_state(RACE_STATE_IDLE);
            }
        }
    } else {
        LOG_INF("BLE disconnected");
        printf("BLE: Disconnected\n");
        if (race.state == RACE_STATE_RUNNING) {
            /* Don't stop the race - keep timing, go to error state */
            set_state(RACE_STATE_ERROR);
            LOG_WRN("BLE lost during race! Timer still running.");
            printf("WARNING: BLE lost during race! Timer still running.\n");
        } else {
            set_state(RACE_STATE_DISCONNECTED);
        }
    }
}

/* ── Race Control ── */

static void handle_start_race(void)
{
    if (race.state != RACE_STATE_CONFIGURED) {
        LOG_WRN("Cannot start race in state %d", race.state);
        send_event(EVT_ERROR, 0, 0);
        return;
    }

    /* If no mode has been set, default to Mode 1 */
    if (race.mode == 0) {
        race.mode = RACE_MODE_4_CONTESTANTS;
    }

    /* Generate race ID from current time */
    race.race_id = (uint32_t)(k_uptime_get() & 0xFFFFFFFF);

    /* Clear results */
    memset(race.results, 0, sizeof(race.results));
    race.results_count = 0;
    race.winner_button = 0;
    race.winner_time_ms = 0;
    race.race_complete = false;

    /* Start the timer */
    race_timer_reset();
    race_timer_start();

    /* Enable button interrupts */
    buttons_enable();

    /* Transition to running state */
    set_state(RACE_STATE_RUNNING);

    /* Send START_ACK to app */
    send_event(EVT_START_ACK, 0, 0);

    LOG_INF("Race started! ID=%u, Mode=%d", race.race_id, race.mode);
    printf("RACE START: ID=%u, Mode=%d (%s)\n", race.race_id, race.mode,
           race.mode == RACE_MODE_4_CONTESTANTS ? "4-contestant" : "1-contestant");
}

static void handle_new_race(void)
{
    LOG_INF("New race / reset");
    printf("RESET: New race / reset\n");

    /* Stop everything */
    race_timer_stop();
    race_timer_reset();
    buttons_disable();

    /* Cancel display updates */
    k_work_cancel_delayable(&display_update_work);

    /* Clear race data */
    memset(&race.results, 0, sizeof(race.results));
    race.results_count = 0;
    race.winner_button = 0;
    race.winner_time_ms = 0;
    race.race_complete = false;
    race.mode = 0;
    race.race_id = 0;

    /* Re-enable all button interrupts so Button 5 stays active */
    buttons_enable();

    /* Return to idle state */
    if (ble_race_service_is_connected()) {
        set_state(RACE_STATE_IDLE);
    } else {
        set_state(RACE_STATE_DISCONNECTED);
    }
}

/* ── Button Event Handler ── */

static void on_button_press(uint8_t button_index, uint32_t timestamp_ms)
{
    LOG_INF("Button %d pressed during state %d", button_index, race.state);
    printf("BUTTON: %d pressed (state=%s)\n", button_index, state_name(race.state));

    /* ── Button 5: Manual Start / Stop ── */
    if (button_index == BUTTON_AUX) {
        handle_button5_press();
        return;
    }

    /* ── Buttons 1-4: Race events (only while running) ── */
    if (race.state != RACE_STATE_RUNNING) {
        LOG_DBG("Ignoring race button %d in non-running state", button_index);
        return;
    }

    if (button_index < 1 || button_index > NUM_RACE_BUTTONS) {
        return;
    }

    handle_button_during_race(button_index);
}

/* ── Button 5: Manual Start / Stop Handler ── */

static void handle_button5_press(void)
{
    LOG_INF("Button 5 (start/stop) pressed in state %d", race.state);

    switch (race.state) {

    case RACE_STATE_DISCONNECTED:
        /*
         * No BLE connection: pressing Button 5 starts a standalone race
         * using default Mode 1 (4 contestants). This allows the mainboard
         * to operate independently without the iOS app.
         */
        LOG_INF("Standalone start (no BLE) — defaulting to Mode 1");
        printf("BTN5: Standalone start (no BLE) — Mode 1\n");
        race.mode = RACE_MODE_4_CONTESTANTS;
        set_state(RACE_STATE_CONFIGURED);
        handle_start_race();
        break;

    case RACE_STATE_IDLE:
        /*
         * BLE connected but no mode set yet: default to Mode 1 and start.
         */
        LOG_INF("Manual start from IDLE — defaulting to Mode 1");
        printf("BTN5: Manual start from IDLE — Mode 1\n");
        race.mode = RACE_MODE_4_CONTESTANTS;
        set_state(RACE_STATE_CONFIGURED);
        handle_start_race();
        break;

    case RACE_STATE_CONFIGURED:
        /*
         * Mode already set: start the race.
         */
        LOG_INF("Manual start from CONFIGURED");
        printf("BTN5: Manual start from CONFIGURED\n");
        handle_start_race();
        break;

    case RACE_STATE_RUNNING:
        /*
         * Race is running: pressing Button 5 stops the race immediately.
         * All recorded results are preserved. If a winner was already
         * determined, that result stands; otherwise the race ends with
         * whatever results have been collected so far.
         */
        LOG_INF("Manual stop — ending race");
        printf("BTN5: Manual stop — ending race\n");
        handle_stop_race();
        break;

    case RACE_STATE_FINISHED:
        /*
         * Race already finished: Button 5 resets for a new race.
         */
        LOG_INF("Manual reset from FINISHED");
        printf("BTN5: Reset from FINISHED\n");
        handle_new_race();
        break;

    case RACE_STATE_ERROR:
        /*
         * Error state: Button 5 forces a full reset.
         */
        LOG_INF("Manual reset from ERROR");
        printf("BTN5: Reset from ERROR\n");
        handle_new_race();
        break;
    }
}

/* ── Manual Stop Race ── */

static void handle_stop_race(void)
{
    uint32_t elapsed = race_timer_get_ms();

    race_timer_stop();
    buttons_disable();

    /* Re-enable button 5 so it can be used to reset */
    buttons_enable();

    /* If no winner was recorded yet, use the current elapsed time */
    if (race.winner_button == 0) {
        if (race.results_count > 0) {
            /* Use the first button that was pressed as the winner */
            race.winner_button = race.results[0].button_index;
            race.winner_time_ms = race.results[0].elapsed_ms;
        } else {
            /* No buttons pressed - display total elapsed time */
            race.winner_time_ms = elapsed;
        }
    }

    race.race_complete = true;

    set_state(RACE_STATE_FINISHED);

    /* Notify iOS app if connected */
    if (race.winner_button > 0) {
        send_event(EVT_RACE_COMPLETE, race.winner_button, race.winner_time_ms);
    } else {
        send_event(EVT_RACE_COMPLETE, 0, elapsed);
    }

    /* Flash button 5 LED briefly as feedback */
    leds_flash_button(BUTTON_AUX);

    LOG_INF("Race manually stopped at %u ms", elapsed);
    if (race.winner_button > 0) {
        printf("RACE STOP: Manual stop at %u ms, winner=Button %d (%u ms)\n",
               elapsed, race.winner_button, race.winner_time_ms);
    } else {
        printf("RACE STOP: Manual stop at %u ms, no winner\n", elapsed);
    }
}

static void handle_button_during_race(uint8_t button_index)
{
    uint32_t elapsed = race_timer_get_ms();

    /* Check for duplicate: has this button already been recorded? */
    for (int i = 0; i < race.results_count; i++) {
        if (race.results[i].button_index == button_index) {
            LOG_WRN("Duplicate stop event for button %d, ignoring", button_index);
            return;
        }
    }

    /* Record the result */
    int idx = race.results_count;
    if (idx >= MAX_RESULTS) {
        LOG_WRN("Max results reached, ignoring button %d", button_index);
        return;
    }

    race.results[idx].button_index = button_index;
    race.results[idx].elapsed_ms = elapsed;
    race.results[idx].recorded = true;
    race.results_count++;

    LOG_INF("Button %d: elapsed=%u ms (result #%d)", button_index, elapsed, idx + 1);
    printf("STOP: Button %d at %u ms (result #%d)\n", button_index, elapsed, idx + 1);

    /* Turn off the LED for this button (visual feedback that it's recorded) */
    leds_set_button(button_index, false);

    /* Send STOP_EVENT to app */
    send_event(EVT_STOP_EVENT, button_index, elapsed);

    /* Check race completion based on mode */
    if (race.mode == RACE_MODE_4_CONTESTANTS) {
        /* Mode 1: First button press = winner */
        if (race.winner_button == 0) {
            race.winner_button = button_index;
            race.winner_time_ms = elapsed;
            LOG_INF("WINNER: Button %d at %u ms!", button_index, elapsed);
            printf("WINNER: Button %d at %u ms!\n", button_index, elapsed);
        }

        /* Race is complete after first press (but we can still record others) */
        if (race.results_count >= 1 && !race.race_complete) {
            race.race_complete = true;

            /* Stop the timer after a brief delay to allow other buttons */
            /* For now, mark complete but keep listening for remaining buttons */
        }

        /* All 4 buttons pressed or timeout → fully finished */
        if (race.results_count >= NUM_RACE_BUTTONS) {
            race_timer_stop();
            buttons_disable();
            set_state(RACE_STATE_FINISHED);
            send_event(EVT_RACE_COMPLETE, race.winner_button, race.winner_time_ms);
            printf("RACE COMPLETE: All buttons pressed. Winner=Button %d (%u ms)\n",
                   race.winner_button, race.winner_time_ms);
        }

    } else if (race.mode == RACE_MODE_1_CONTESTANT) {
        /* Mode 2: Sequential segments, all 4 must be completed */
        if (race.results_count >= NUM_RACE_BUTTONS) {
            /* All 4 segments completed */
            race.race_complete = true;
            race.winner_button = button_index;  /* Last segment button */
            race.winner_time_ms = elapsed;       /* Total elapsed time */

            race_timer_stop();
            buttons_disable();
            set_state(RACE_STATE_FINISHED);
            send_event(EVT_RACE_COMPLETE, 0, elapsed);
            LOG_INF("All segments complete! Total time: %u ms", elapsed);
            printf("RACE COMPLETE: All segments done! Total time: %u ms\n", elapsed);
        }
    }
}

/* ── BLE Event Sender ── */

static const char *event_name(uint8_t evt_type)
{
    switch (evt_type) {
    case EVT_START_ACK:    return "START_ACK";
    case EVT_STOP_EVENT:   return "STOP_EVENT";
    case EVT_RACE_COMPLETE: return "RACE_COMPLETE";
    case EVT_STATUS:       return "STATUS";
    case EVT_PONG:         return "PONG";
    case EVT_ERROR:        return "ERROR";
    default:               return "UNKNOWN";
    }
}

static void send_event(uint8_t event_type, uint8_t button_index, uint32_t elapsed_ms)
{
    struct race_event_packet evt = {
        .event_type = event_type,
        .mode = race.mode,
        .button_index = button_index,
        .elapsed_time_ms = elapsed_ms,
        .race_id = race.race_id,
    };

    printf("EVT: %s btn=%d elapsed=%u ms race_id=%u\n",
           event_name(event_type), button_index, elapsed_ms, race.race_id);

    int err = ble_race_service_notify(&evt);
    if (err) {
        LOG_WRN("Failed to send event 0x%02X (err %d)", event_type, err);
    }
}

/* ── Display Update ── */

static void display_update_handler(struct k_work *work)
{
    if (race.state != RACE_STATE_RUNNING) {
        return;
    }

    /* Update display with current elapsed time */
    uint32_t elapsed = race_timer_get_ms();
    display_time(elapsed, true);  /* MM:SS format */

    /* Reschedule for next update */
    k_work_reschedule(&display_update_work, K_MSEC(DISPLAY_UPDATE_INTERVAL_MS));
}

/* ── Public API ── */

race_state_t race_manager_get_state(void)
{
    return race.state;
}

const struct race_data *race_manager_get_data(void)
{
    return &race;
}

void race_manager_process(void)
{
}

