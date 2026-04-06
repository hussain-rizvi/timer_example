/*
 * BLE Race Timer Service Implementation
 *
 * Custom GATT service using Zephyr BLE stack for communication with the
 * iOS race timer application.
 */

#include <errno.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble_race_service.h"

LOG_MODULE_REGISTER(ble_race, LOG_LEVEL_INF);

#define BT_UUID_RACE_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x00001000, 0x7261, 0x6365, 0x7469, 0x6d6572303031)
#define BT_UUID_RACE_COMMAND_VAL \
    BT_UUID_128_ENCODE(0x00001001, 0x7261, 0x6365, 0x7469, 0x6d6572303031)
#define BT_UUID_RACE_EVENT_VAL \
    BT_UUID_128_ENCODE(0x00001002, 0x7261, 0x6365, 0x7469, 0x6d6572303031)
#define BT_UUID_RACE_STATUS_VAL \
    BT_UUID_128_ENCODE(0x00001003, 0x7261, 0x6365, 0x7469, 0x6d6572303031)

static struct bt_uuid_128 race_service_uuid = BT_UUID_INIT_128(BT_UUID_RACE_SERVICE_VAL);
static struct bt_uuid_128 race_cmd_uuid = BT_UUID_INIT_128(BT_UUID_RACE_COMMAND_VAL);
static struct bt_uuid_128 race_evt_uuid = BT_UUID_INIT_128(BT_UUID_RACE_EVENT_VAL);
static struct bt_uuid_128 race_status_uuid = BT_UUID_INIT_128(BT_UUID_RACE_STATUS_VAL);

static const struct ble_race_service_cb *service_cb;
static struct bt_conn *current_conn;
static bool notifications_enabled;
static uint8_t race_status_value;

static void evt_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Event notifications %s", notifications_enabled ? "enabled" : "disabled");
}

static ssize_t on_cmd_write(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len,
                            uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0U) {
        LOG_WRN("Rejecting command write with non-zero offset: %u", offset);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len != sizeof(struct race_cmd_packet)) {
        LOG_WRN("Invalid command packet size: %u bytes", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (service_cb && service_cb->on_command) {
        const struct race_cmd_packet *cmd = buf;
        LOG_INF("Received command: type=0x%02X, mode=0x%02X", cmd->cmd_type, cmd->mode);
        service_cb->on_command(cmd);
    }

    return len;
}

static ssize_t on_status_read(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len,
                              uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &race_status_value, sizeof(race_status_value));
}

BT_GATT_SERVICE_DEFINE(race_svc,
    BT_GATT_PRIMARY_SERVICE(&race_service_uuid),
    BT_GATT_CHARACTERISTIC(&race_cmd_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, on_cmd_write, NULL),
    BT_GATT_CHARACTERISTIC(&race_evt_uuid.uuid,
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE,
                           NULL, NULL, NULL),
    BT_GATT_CCC(evt_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&race_status_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           on_status_read, NULL, &race_status_value),
);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_RACE_SERVICE_VAL),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, "RaceTimer", sizeof("RaceTimer") - 1),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }

    if (current_conn != NULL) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    current_conn = bt_conn_ref(conn);
    LOG_INF("BLE connected");

    if (service_cb && service_cb->on_connection_change) {
        service_cb->on_connection_change(true);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (reason %u)", reason);

    if (current_conn == conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    } else if (current_conn != NULL) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    notifications_enabled = false;

    if (service_cb && service_cb->on_connection_change) {
        service_cb->on_connection_change(false);
    }

    (void)ble_race_service_start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

int ble_race_service_init(const struct ble_race_service_cb *callbacks)
{
    int err;

    service_cb = callbacks;
    notifications_enabled = false;
    race_status_value = 0U;

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return err;
    }

    LOG_INF("Bluetooth initialized");
    return 0;
}

int ble_race_service_start_advertising(void)
{
    int err;

    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err == -EALREADY) {
        LOG_DBG("Advertising already active");
        return 0;
    }

    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return err;
    }

    LOG_INF("Advertising started");
    return 0;
}

int ble_race_service_notify(const struct race_event_packet *evt)
{
    const struct bt_gatt_attr *evt_attr = &race_svc.attrs[4];

    if (evt == NULL) {
        return -EINVAL;
    }

    if (current_conn == NULL) {
        return -ENOTCONN;
    }

    if (!notifications_enabled) {
        return -EACCES;
    }

    return bt_gatt_notify(current_conn, evt_attr, evt, sizeof(*evt));
}

bool ble_race_service_is_connected(void)
{
    return (current_conn != NULL);
}

void ble_race_service_set_status(uint8_t status)
{
    race_status_value = status;
}
