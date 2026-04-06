/*
 * Race Manager Implementation
 *
 * Central coordination of all race timer subsystems.
 * Handles the race state machine, BLE commands, button events,
 * display updates, and LED indicators.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble_race_service.h"
#include "buttons.h"
#include "display.h"
#include "leds.h"
#include "race_manager.h"
#include "race_timer.h"

LOG_MODULE_REGISTER(race_mgr, LOG_LEVEL_INF);

#define DISPLAY_UPDATE_INTERVAL_MS 10
#define ERROR_BLINK_DURATION_MS    (10 * (100 + 100))
#define BUTTON_AUX                 5U

static struct race_data race;
static struct k_work_delayable display_update_work;
static struct k_work_delayable error_blink_done_work;

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
static void error_blink_done_handler(struct k_work *work);
static void update_leds_for_state(void);
static void reset_results(void);
static bool race_mode_is_valid(uint8_t mode);

static const struct ble_race_service_cb ble_callbacks = {
    .on_command = on_ble_command,
    .on_connection_change = on_ble_connection_change,
};

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

static const char *event_name(uint8_t evt_type)
{
    switch (evt_type) {
    case EVT_START_ACK:     return "START_ACK";
    case EVT_STOP_EVENT:    return "STOP_EVENT";
    case EVT_RACE_COMPLETE: return "RACE_COMPLETE";
    case EVT_STATUS:        return "STATUS";
    case EVT_PONG:          return "PONG";
    case EVT_ERROR:         return "ERROR";
    default:                return "UNKNOWN";
    }
}

static bool race_mode_is_valid(uint8_t mode)
{
    return (mode == RACE_MODE_4_CONTESTANTS) || (mode == RACE_MODE_1_CONTESTANT);
}

static void reset_results(void)
{
    memset(race.results, 0, sizeof(race.results));
    race.results_count = 0U;
    race.winner_button = 0U;
    race.winner_time_ms = 0U;
    race.race_complete = false;
}

int race_manager_init(void)
{
    int err;

    memset(&race, 0, sizeof(race));
    race.state = RACE_STATE_DISCONNECTED;

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
        LOG_WRN("Continuing without display");
    }

    err = race_timer_init();
    if (err) {
        LOG_ERR("Timer init failed (err %d)", err);
        return err;
    }

    err = ble_race_service_init(&ble_callbacks);
    if (err) {
        LOG_ERR("BLE init failed (err %d)", err);
        return err;
    }

    err = ble_race_service_start_advertising();
    if (err) {
        LOG_ERR("Advertising start failed (err %d)", err);
        return err;
    }

    k_work_init_delayable(&display_update_work, display_update_handler);
    k_work_init_delayable(&error_blink_done_work, error_blink_done_handler);

    buttons_enable();
    display_time(0U, true);
    leds_set_status(false);
    leds_status_blink(200U, 800U, 0U);

    LOG_INF("Race manager initialized");
    printf("\n");
    printf("Race manager initialized OK\n");
    printf("  Buttons: 4 race + 1 aux (start/stop)\n");
    printf("  Modes: 1=4-contestant, 2=1-contestant\n");
    printf("  Press Button 5 to start standalone race\n");
    return 0;
}

static void set_state(race_state_t new_state)
{
    if (race.state == new_state) {
        return;
    }

    if (race.state == RACE_STATE_RUNNING) {
        k_work_cancel_delayable(&display_update_work);
    }

    if (race.state == RACE_STATE_ERROR) {
        k_work_cancel_delayable(&error_blink_done_work);
    }

    LOG_INF("State: %d -> %d", race.state, new_state);
    printf("STATE: %s -> %s\n", state_name(race.state), state_name(new_state));

    race.state = new_state;
    ble_race_service_set_status((uint8_t)new_state);
    update_leds_for_state();
}

static void update_leds_for_state(void)
{
    leds_stop_blink_button();

    switch (race.state) {
    case RACE_STATE_DISCONNECTED:
        leds_all_off();
        leds_status_blink(200U, 800U, 0U);
        display_time(0U, true);
        break;

    case RACE_STATE_IDLE:
        leds_all_off();
        leds_set_status(true);
        display_time(0U, true);
        break;

    case RACE_STATE_CONFIGURED:
        leds_all_off();
        for (uint8_t i = 1U; i <= NUM_RACE_BUTTONS; ++i) {
            leds_set_button(i, true);
        }
        leds_set_status(true);
        display_time(0U, true);
        break;

    case RACE_STATE_RUNNING:
        leds_set_status(true);
        k_work_reschedule(&display_update_work, K_NO_WAIT);
        break;

    case RACE_STATE_FINISHED:
        leds_all_off();
        if (race.mode == RACE_MODE_1_CONTESTANT) {
            leds_blink_all_buttons(300U, 300U);
        } else if ((race.winner_button >= 1U) && (race.winner_button <= NUM_BUTTON_LEDS)) {
            leds_blink_button(race.winner_button, 300U, 300U);
        }
        leds_set_status(true);
        display_time(race.winner_time_ms, true);
        break;

    case RACE_STATE_ERROR:
        leds_all_off();
        leds_status_blink(100U, 100U, 10U);
        display_time(0U, true);
        k_work_reschedule(&error_blink_done_work, K_MSEC(ERROR_BLINK_DURATION_MS));
        break;
    }
}

static void on_ble_command(const struct race_cmd_packet *cmd)
{
    if (cmd == NULL) {
        return;
    }

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
        if ((race.state != RACE_STATE_IDLE) && (race.state != RACE_STATE_CONFIGURED)) {
            LOG_WRN("Cannot set mode in state %d", race.state);
            printf("WARNING: Cannot set mode in state %s\n", state_name(race.state));
            send_event(EVT_ERROR, ERR_REASON_MODE_NOT_ALLOWED, 0U);
            break;
        }

        if (!race_mode_is_valid(cmd->mode)) {
            LOG_WRN("Invalid race mode: %u", cmd->mode);
            printf("WARNING: Invalid race mode %u\n", cmd->mode);
            send_event(EVT_ERROR, ERR_REASON_INVALID_STATE, 0U);
            break;
        }

        race.mode = cmd->mode;
        set_state(RACE_STATE_CONFIGURED);
        LOG_INF("Mode set to %d", race.mode);
        printf("MODE: Set to %d (%s)\n", race.mode,
               (race.mode == RACE_MODE_4_CONTESTANTS) ? "4-contestant" : "1-contestant");
        break;

    case CMD_PING:
        printf("PING: Received, sending PONG\n");
        send_event(EVT_PONG, 0U, 0U);
        break;

    case CMD_GET_STATUS:
        printf("STATUS: Requested, elapsed=%u ms\n", race_timer_get_ms());
        send_event(EVT_STATUS, 0U, race_timer_get_ms());
        break;

    default:
        LOG_WRN("Unknown command: 0x%02X", cmd->cmd_type);
        printf("WARNING: Unknown command 0x%02X\n", cmd->cmd_type);
        send_event(EVT_ERROR, ERR_REASON_UNKNOWN_CMD, 0U);
        break;
    }
}

static void on_ble_connection_change(bool connected)
{
    if (connected) {
        LOG_INF("BLE connected");
        printf("BLE: Connected\n");

        if (race.state == RACE_STATE_DISCONNECTED) {
            set_state(RACE_STATE_IDLE);
        } else if (race.state == RACE_STATE_ERROR) {
            if (race_timer_is_running()) {
                set_state(RACE_STATE_RUNNING);
                send_event(EVT_STATUS, 0U, race_timer_get_ms());
            } else {
                set_state(RACE_STATE_IDLE);
            }
        }
        return;
    }

    LOG_INF("BLE disconnected");
    printf("BLE: Disconnected\n");

    if (race.state == RACE_STATE_RUNNING) {
        set_state(RACE_STATE_ERROR);
        LOG_WRN("BLE lost during race. Timer still running.");
        printf("WARNING: BLE lost during race. Timer still running.\n");
    } else {
        set_state(RACE_STATE_DISCONNECTED);
    }
}

static void handle_start_race(void)
{
    if (race.state != RACE_STATE_CONFIGURED) {
        LOG_WRN("Cannot start race in state %d", race.state);
        send_event(EVT_ERROR, ERR_REASON_INVALID_STATE, 0U);
        return;
    }

    if (!race_mode_is_valid(race.mode)) {
        race.mode = RACE_MODE_4_CONTESTANTS;
    }

    race.race_id = (uint32_t)(k_uptime_get() & 0xFFFFFFFFu);
    reset_results();

    race_timer_reset();
    race_timer_start();
    buttons_enable();
    set_state(RACE_STATE_RUNNING);
    send_event(EVT_START_ACK, 0U, 0U);

    LOG_INF("Race started. ID=%u, Mode=%d", race.race_id, race.mode);
    printf("RACE START: ID=%u, Mode=%d (%s)\n", race.race_id, race.mode,
           (race.mode == RACE_MODE_4_CONTESTANTS) ? "4-contestant" : "1-contestant");
}

static void handle_new_race(void)
{
    race_state_t prev_state = race.state;

    LOG_INF("New race / reset");
    printf("RESET: New race / reset\n");

    race_timer_stop();
    race_timer_reset();
    buttons_disable();
    k_work_cancel_delayable(&display_update_work);
    reset_results();
    race.mode = 0U;
    race.race_id = 0U;

    buttons_enable();

    if (prev_state == RACE_STATE_ERROR) {
        set_state(RACE_STATE_DISCONNECTED);
    } else if (ble_race_service_is_connected()) {
        set_state(RACE_STATE_IDLE);
    } else {
        set_state(RACE_STATE_DISCONNECTED);
    }
}

static void on_button_press(uint8_t button_index, uint32_t timestamp_ms)
{
    ARG_UNUSED(timestamp_ms);

    LOG_INF("Button %u pressed during state %d", button_index, race.state);
    printf("BUTTON: %u pressed (state=%s)\n", button_index, state_name(race.state));

    if (button_index == BUTTON_AUX) {
        handle_button5_press();
        return;
    }

    if (race.state != RACE_STATE_RUNNING) {
        LOG_DBG("Ignoring race button %u in non-running state", button_index);
        return;
    }

    if ((button_index < 1U) || (button_index > NUM_RACE_BUTTONS)) {
        return;
    }

    handle_button_during_race(button_index);
}

static void handle_button5_press(void)
{
    LOG_INF("Button 5 (start/stop) pressed in state %d", race.state);

    switch (race.state) {
    case RACE_STATE_DISCONNECTED:
        LOG_INF("Standalone start (no BLE), defaulting to Mode 1");
        printf("BTN5: Standalone start (no BLE), Mode 1\n");
        race.mode = RACE_MODE_4_CONTESTANTS;
        set_state(RACE_STATE_CONFIGURED);
        handle_start_race();
        break;

    case RACE_STATE_IDLE:
        LOG_INF("Manual start from IDLE, defaulting to Mode 1");
        printf("BTN5: Manual start from IDLE, Mode 1\n");
        race.mode = RACE_MODE_4_CONTESTANTS;
        set_state(RACE_STATE_CONFIGURED);
        handle_start_race();
        break;

    case RACE_STATE_CONFIGURED:
        LOG_INF("Manual start from CONFIGURED");
        printf("BTN5: Manual start from CONFIGURED\n");
        handle_start_race();
        break;

    case RACE_STATE_RUNNING:
        LOG_INF("Manual stop, ending race");
        printf("BTN5: Manual stop, ending race\n");
        handle_stop_race();
        break;

    case RACE_STATE_FINISHED:
        LOG_INF("Manual reset from FINISHED");
        printf("BTN5: Reset from FINISHED\n");
        handle_new_race();
        break;

    case RACE_STATE_ERROR:
        LOG_INF("Manual reset from ERROR");
        printf("BTN5: Reset from ERROR\n");
        handle_new_race();
        break;
    }
}

static void handle_stop_race(void)
{
    uint32_t elapsed = race_timer_get_ms();

    race_timer_stop();
    buttons_disable();
    buttons_enable();

    if (race.winner_button == 0U) {
        if (race.results_count > 0U) {
            race.winner_button = race.results[0].button_index;
            race.winner_time_ms = race.results[0].elapsed_ms;
        } else {
            race.winner_time_ms = elapsed;
        }
    }

    race.race_complete = true;
    set_state(RACE_STATE_FINISHED);

    if (race.winner_button > 0U) {
        send_event(EVT_RACE_COMPLETE, race.winner_button, race.winner_time_ms);
    } else {
        send_event(EVT_RACE_COMPLETE, 0U, elapsed);
    }

    LOG_INF("Race manually stopped at %u ms", elapsed);
    if (race.winner_button > 0U) {
        printf("RACE STOP: Manual stop at %u ms, winner=Button %u (%u ms)\n",
               elapsed, race.winner_button, race.winner_time_ms);
    } else {
        printf("RACE STOP: Manual stop at %u ms, no winner\n", elapsed);
    }
}

static void handle_button_during_race(uint8_t button_index)
{
    uint32_t elapsed = race_timer_get_ms();
    uint8_t idx;

    for (uint8_t i = 0U; i < race.results_count; ++i) {
        if (race.results[i].button_index == button_index) {
            LOG_WRN("Duplicate stop event for button %u, ignoring", button_index);
            return;
        }
    }

    idx = race.results_count;
    if (idx >= MAX_RESULTS) {
        LOG_WRN("Max results reached, ignoring button %u", button_index);
        return;
    }

    race.results[idx].button_index = button_index;
    race.results[idx].elapsed_ms = elapsed;
    race.results[idx].recorded = true;
    race.results_count++;

    LOG_INF("Button %u: elapsed=%u ms (result #%u)", button_index, elapsed, idx + 1U);
    printf("STOP: Button %u at %u ms (result #%u)\n", button_index, elapsed, idx + 1U);

    leds_set_button(button_index, false);
    send_event(EVT_STOP_EVENT, button_index, elapsed);

    if (race.mode == RACE_MODE_4_CONTESTANTS) {
        if (race.winner_button == 0U) {
            race.winner_button = button_index;
            race.winner_time_ms = elapsed;
            LOG_INF("WINNER: Button %u at %u ms", button_index, elapsed);
            printf("WINNER: Button %u at %u ms\n", button_index, elapsed);
        }

        if (!race.race_complete) {
            race.race_complete = true;
        }

        if (race.results_count >= NUM_RACE_BUTTONS) {
            race_timer_stop();
            buttons_disable();
            buttons_enable();
            set_state(RACE_STATE_FINISHED);
            send_event(EVT_RACE_COMPLETE, race.winner_button, race.winner_time_ms);
            printf("RACE COMPLETE: All buttons pressed. Winner=Button %u (%u ms)\n",
                   race.winner_button, race.winner_time_ms);
        }
        return;
    }

    if (race.mode == RACE_MODE_1_CONTESTANT) {
        if (race.results_count >= NUM_RACE_BUTTONS) {
            race.race_complete = true;
            race.winner_button = button_index;
            race.winner_time_ms = elapsed;

            race_timer_stop();
            buttons_disable();
            buttons_enable();
            set_state(RACE_STATE_FINISHED);
            send_event(EVT_RACE_COMPLETE, 0U, elapsed);
            LOG_INF("All segments complete. Total time: %u ms", elapsed);
            printf("RACE COMPLETE: All segments done. Total time: %u ms\n", elapsed);
        }
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

    if (!ble_race_service_is_connected()) {
        LOG_DBG("Skipping BLE event %s because no client is connected", event_name(event_type));
        return;
    }

    printf("EVT: %s btn=%u elapsed=%u ms race_id=%u\n",
           event_name(event_type), button_index, elapsed_ms, race.race_id);

    int err = ble_race_service_notify(&evt);
    if (err) {
        LOG_WRN("Failed to send event 0x%02X (err %d)", event_type, err);
    }
}

static void error_blink_done_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (race.state == RACE_STATE_ERROR) {
        set_state(RACE_STATE_DISCONNECTED);
    }
}

static void display_update_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (race.state != RACE_STATE_RUNNING) {
        return;
    }

    display_time(race_timer_get_ms(), true);
    k_work_reschedule(&display_update_work, K_MSEC(DISPLAY_UPDATE_INTERVAL_MS));
}

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
