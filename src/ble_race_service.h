/*
 * BLE Race Timer Service
 *
 * Custom GATT service for race timer communication with iOS app.
 *
 * Service UUID: 00001000-7261-6365-7469-6d6572303031
 *   - Command Characteristic (Write):  00001001-...  (app → mainboard)
 *   - Event Characteristic (Notify):   00001002-...  (mainboard → app)
 *   - Status Characteristic (Read):    00001003-...  (race state)
 */

#ifndef BLE_RACE_SERVICE_H_
#define BLE_RACE_SERVICE_H_

#include <zephyr/types.h>
#include <zephyr/bluetooth/conn.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── BLE Protocol Command IDs (app → mainboard) ── */
#define CMD_START_RACE   0x01
#define CMD_NEW_RACE     0x02
#define CMD_RESET        0x03
#define CMD_PING         0x04
#define CMD_GET_STATUS   0x05
#define CMD_SET_MODE     0x06

/* ── BLE Protocol Event IDs (mainboard → app) ── */
#define EVT_START_ACK    0x10
#define EVT_STOP_EVENT   0x11
#define EVT_RACE_COMPLETE 0x12
#define EVT_STATUS       0x13
#define EVT_PONG         0x14
#define EVT_ERROR        0x1F

/* ── Error Reason Codes (sent in button_index field of EVT_ERROR) ── */
#define ERR_REASON_UNKNOWN_CMD       0x01  /* Unrecognised command type */
#define ERR_REASON_INVALID_STATE     0x02  /* Command not allowed in current state */
#define ERR_REASON_MODE_NOT_ALLOWED  0x03  /* Cannot set mode in current state */

/* ── Race Modes ── */
#define RACE_MODE_4_CONTESTANTS  0x01
#define RACE_MODE_1_CONTESTANT   0x02

/* ── Event Packet Structure (mainboard → app) ── */
struct __packed race_event_packet {
    uint8_t  event_type;      /* EVT_* */
    uint8_t  mode;            /* RACE_MODE_* */
    uint8_t  button_index;    /* 1-4 (0 if N/A) */
    uint32_t elapsed_time_ms; /* Elapsed time in milliseconds */
    uint32_t race_id;         /* Race identifier (timestamp) */
};

/* ── Command Packet Structure (app → mainboard) ── */
struct __packed race_cmd_packet {
    uint8_t  cmd_type;        /* CMD_* */
    uint8_t  mode;            /* RACE_MODE_* (for CMD_SET_MODE) */
    uint8_t  reserved[2];     /* Padding */
};

/* Callback for received commands */
typedef void (*ble_race_cmd_cb_t)(const struct race_cmd_packet *cmd);

/* Callback for connection state changes */
typedef void (*ble_race_conn_cb_t)(bool connected);

struct ble_race_service_cb {
    ble_race_cmd_cb_t   on_command;
    ble_race_conn_cb_t  on_connection_change;
};

/**
 * @brief Initialize the BLE Race Timer service.
 *
 * @param callbacks Pointer to callback structure.
 * @return 0 on success, negative errno on failure.
 */
int ble_race_service_init(const struct ble_race_service_cb *callbacks);

/**
 * @brief Send an event notification to the connected iOS app.
 *
 * @param evt Pointer to event packet to send.
 * @return 0 on success, negative errno on failure.
 */
int ble_race_service_notify(const struct race_event_packet *evt);

/**
 * @brief Check if a BLE client is connected.
 *
 * @return true if connected, false otherwise.
 */
bool ble_race_service_is_connected(void);

/**
 * @brief Update the race status characteristic value.
 *
 * @param status Current race state (race_state_t cast to uint8_t).
 */
void ble_race_service_set_status(uint8_t status);

/**
 * @brief Start BLE advertising.
 *
 * @return 0 on success, negative errno on failure.
 */
int ble_race_service_start_advertising(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_RACE_SERVICE_H_ */

