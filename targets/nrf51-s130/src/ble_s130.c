#include "od_nrf51_ble.h"

#include "od_nrf51_long_write.h"
#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"

#ifdef OD_NRF51_BLE_HOST_TEST
#include "fake_s130.h"
#else
#include <ble.h>
#include <ble_gap.h>
#include <ble_gatt.h>
#include <ble_gatts.h>
#include <ble_hci.h>
#include <nrf.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define OD_SERVICE_UUID 0x2446u
#define OD_ATT_BUSY (BLE_GATT_STATUS_ATTERR_APP_BEGIN + 0u)
#define OD_NRF51_BLE_EVENT_BUFFER_SIZE 128u

/* S130 event structs include one write byte; reserve the other MTU-23 value bytes explicitly. */
typedef char od_nrf51_ble_event_buffer_must_fit[
    (sizeof(ble_evt_t) + OD_NRF51_NOTIFY_MAX - 1u <= OD_NRF51_BLE_EVENT_BUFFER_SIZE) ? 1 : -1];

static uint16_t connection = BLE_CONN_HANDLE_INVALID;
static ble_gatts_char_handles_t characteristic;
static od_nrf51_long_write_t long_write;
static bool subscribed;
static bool healthy;
static bool reboot_flag;
static uint8_t loop_counter;
static uint8_t msd[16];

volatile uint32_t od_nrf51_boot_error;
volatile uint32_t od_nrf51_required_ram_start;

static void fatal(uint32_t error)
{
    if (error == NRF_SUCCESS) {
        return;
    }
    od_nrf51_boot_error = error;
    healthy = false;
}

#ifndef OD_NRF51_BLE_HOST_TEST
static void softdevice_assert(uint32_t id, uint32_t pc, uint32_t info)
{
    (void)id;
    (void)pc;
    (void)info;
    NVIC_SystemReset();
}
#endif

static void update_msd(void)
{
    int32_t quarter_degrees = 0;
    int32_t encoded_temperature = 0;

    memset(msd, 0, sizeof(msd));
    msd[0] = 0x46u;
    msd[1] = 0x24u;
    if (sd_temp_get(&quarter_degrees) == NRF_SUCCESS) {
        encoded_temperature = quarter_degrees / 2 + 80;
        if (encoded_temperature < 0) {
            encoded_temperature = 0;
        } else if (encoded_temperature > 255) {
            encoded_temperature = 255;
        }
        msd[13] = (uint8_t)encoded_temperature;
    }
    /* The wire format has no unknown-voltage encoding; ADC qualification is still open. */
    msd[14] = 0u;
    msd[15] = (uint8_t)((reboot_flag ? 0x02u : 0u) |
                        ((loop_counter & 0x0Fu) << 4));
}

void od_nrf51_copy_msd(uint8_t output[16])
{
    update_msd();
    memcpy(output, msd, sizeof(msd));
}

#ifndef OD_NRF51_BLE_HOST_TEST
static void advertise(void)
{
    uint8_t adv[] = {
        2u, BLE_GAP_AD_TYPE_FLAGS, BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE,
        3u, BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_COMPLETE, 0x46u, 0x24u,
        17u, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    uint8_t scan_response[] = {
        15u, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME,
        'O', 'D', '-', 'L', 'T', '2', '1', '3', 'A', '-', '0', '0', '0', '0'
    };
    ble_gap_adv_params_t params;
    static const char hex[] = "0123456789ABCDEF";
    const uint16_t suffix = (uint16_t)NRF_FICR->DEVICEID[0];

    update_msd();
    memcpy(adv + 9u, msd, sizeof(msd));
    scan_response[12] = (uint8_t)hex[(suffix >> 12) & 0xFu];
    scan_response[13] = (uint8_t)hex[(suffix >> 8) & 0xFu];
    scan_response[14] = (uint8_t)hex[(suffix >> 4) & 0xFu];
    scan_response[15] = (uint8_t)hex[suffix & 0xFu];
    memset(&params, 0, sizeof(params));
    params.type = BLE_GAP_ADV_TYPE_ADV_IND;
    params.fp = BLE_GAP_ADV_FP_ANY;
    params.interval = 800u;
    fatal(sd_ble_gap_adv_data_set(adv, sizeof(adv), scan_response,
                                  sizeof(scan_response)));
    if (healthy) {
        fatal(sd_ble_gap_adv_start(&params));
    }
}

static void service_init(void)
{
    ble_uuid_t uuid = { OD_SERVICE_UUID, BLE_UUID_TYPE_BLE };
    ble_gap_conn_sec_mode_t security;
    ble_gatts_attr_md_t cccd;
    ble_gatts_char_md_t characteristic_md;
    ble_gatts_attr_md_t value_md;
    ble_gatts_attr_t value;
    uint16_t service_handle = 0u;

    memset(&cccd, 0, sizeof(cccd));
    cccd.vloc = BLE_GATTS_VLOC_STACK;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd.write_perm);
    memset(&characteristic_md, 0, sizeof(characteristic_md));
    characteristic_md.char_props.write = 1u;
    characteristic_md.char_props.notify = 1u;
    characteristic_md.p_cccd_md = &cccd;
    memset(&value_md, 0, sizeof(value_md));
    value_md.vloc = BLE_GATTS_VLOC_STACK;
    value_md.vlen = 1u;
    value_md.wr_auth = 1u;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&value_md.write_perm);
    memset(&value, 0, sizeof(value));
    value.p_uuid = &uuid;
    value.p_attr_md = &value_md;
    value.max_len = OD_NRF51_MAX_FRAME;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&security);
    fatal(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &uuid, &service_handle));
    fatal(sd_ble_gatts_characteristic_add(service_handle, &characteristic_md,
                                          &value, &characteristic));
}

void od_nrf51_ble_init(void)
{
    const nrf_clock_lf_cfg_t clock = { NRF_CLOCK_LF_SRC_RC, 4u, 4u, 0u };
    ble_enable_params_t enable;
    ble_gap_conn_sec_mode_t security;
    ble_gap_conn_params_t timing;
    char name[] = "OD-LT213A-0000";
    static const char hex[] = "0123456789ABCDEF";
    uint32_t ram_start = 0x20001F00u;
    const uint16_t suffix = (uint16_t)NRF_FICR->DEVICEID[0];

    healthy = true;
    od_nrf51_long_write_reset(&long_write);
    fatal(sd_softdevice_enable(&clock, softdevice_assert));
    memset(&enable, 0, sizeof(enable));
    enable.gatts_enable_params.attr_tab_size = 1024u;
    enable.gatts_enable_params.service_changed = 0u;
    enable.gap_enable_params.periph_conn_count = 1u;
    if (healthy) {
        fatal(sd_ble_enable(&enable, &ram_start));
    }
    od_nrf51_required_ram_start = ram_start;
    if (!healthy || ram_start > 0x20001F00u) {
        fatal(NRF_ERROR_NO_MEM);
        return;
    }
    reboot_flag = true;
    loop_counter = 0u;
    name[10] = hex[(suffix >> 12) & 0xFu];
    name[11] = hex[(suffix >> 8) & 0xFu];
    name[12] = hex[(suffix >> 4) & 0xFu];
    name[13] = hex[suffix & 0xFu];
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&security);
    fatal(sd_ble_gap_device_name_set(&security, (const uint8_t *)name,
                                     (uint16_t)(sizeof(name) - 1u)));
    memset(&timing, 0, sizeof(timing));
    timing.min_conn_interval = 24u;
    timing.max_conn_interval = 40u;
    timing.slave_latency = 0u;
    timing.conn_sup_timeout = 1200u;
    fatal(sd_ble_gap_ppcp_set(&timing));
    fatal(sd_ble_gap_tx_power_set(0));
    service_init();
    if (healthy) {
        advertise();
    }
}
#else
static void advertise(void)
{
}
#endif

static void authorize_reply(const ble_gatts_evt_write_t *write, uint16_t status,
                            bool update)
{
    ble_gatts_rw_authorize_reply_params_t reply;
    uint32_t result = NRF_ERROR_BUSY;

    memset(&reply, 0, sizeof(reply));
    reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
    reply.params.write.gatt_status = status;
    reply.params.write.update = update ? 1u : 0u;
    if (update) {
        reply.params.write.offset = write->offset;
        reply.params.write.len = write->len;
        reply.params.write.p_data = write->data;
    }
    for (uint8_t attempt = 0u; attempt < OD_NRF51_AUTH_REPLY_ATTEMPTS; ++attempt) {
        result = sd_ble_gatts_rw_authorize_reply(connection, &reply);
        if (result != NRF_ERROR_BUSY) {
            break;
        }
    }
    fatal(result);
}

static void authorize_write(const ble_gatts_evt_write_t *write)
{
    const uint8_t *frame = NULL;
    uint16_t frame_length = 0u;

    if (!subscribed || write->handle != characteristic.value_handle) {
        od_nrf51_long_write_reset(&long_write);
        authorize_reply(write, BLE_GATT_STATUS_ATTERR_CPS_CCCD_CONFIG_ERROR, false);
        return;
    }
    if (write->op == BLE_GATTS_OP_PREP_WRITE_REQ) {
        if (!od_nrf51_slim_admission_free() ||
            (long_write.active && write->offset == 0u)) {
            authorize_reply(write, OD_ATT_BUSY, false);
            return;
        }
        if (od_nrf51_long_write_prepare(&long_write, characteristic.value_handle,
                                        write->handle, write->offset, write->data,
                                        write->len) != OD_NRF51_LW_ACCEPTED) {
            authorize_reply(write, BLE_GATT_STATUS_ATTERR_INVALID_ATT_VAL_LENGTH, false);
            return;
        }
        authorize_reply(write, BLE_GATT_STATUS_SUCCESS, false);
        return;
    }
    if (write->op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL) {
        od_nrf51_long_write_reset(&long_write);
        authorize_reply(write, BLE_GATT_STATUS_SUCCESS, false);
        return;
    }
    if (write->op == BLE_GATTS_OP_EXEC_WRITE_REQ_NOW) {
        if (!od_nrf51_slim_admission_free() ||
            od_nrf51_long_write_execute(&long_write, true, &frame, &frame_length) !=
                OD_NRF51_LW_READY ||
            !od_nrf51_slim_receive(frame, frame_length)) {
            od_nrf51_long_write_reset(&long_write);
            authorize_reply(write, OD_ATT_BUSY, false);
            return;
        }
        authorize_reply(write, BLE_GATT_STATUS_SUCCESS, false);
        return;
    }
    if (write->op == BLE_GATTS_OP_WRITE_REQ) {
        if (long_write.active || !od_nrf51_slim_admission_free()) {
            authorize_reply(write, OD_ATT_BUSY, false);
        } else if (write->offset != 0u || write->len == 0u || write->len > 20u ||
                   !od_nrf51_slim_receive(write->data, write->len)) {
            authorize_reply(write, BLE_GATT_STATUS_ATTERR_INVALID_ATT_VAL_LENGTH, false);
        } else {
            authorize_reply(write, BLE_GATT_STATUS_SUCCESS, true);
        }
        return;
    }
    authorize_reply(write, BLE_GATT_STATUS_ATTERR_REQUEST_NOT_SUPPORTED, false);
}

static void handle_event(const ble_evt_t *event)
{
    switch (event->header.evt_id) {
    case BLE_GAP_EVT_CONNECTED:
        connection = event->evt.gap_evt.conn_handle;
        subscribed = false;
        reboot_flag = false;
        od_nrf51_long_write_reset(&long_write);
        od_nrf51_slim_reset();
        break;
    case BLE_GAP_EVT_DISCONNECTED:
        connection = BLE_CONN_HANDLE_INVALID;
        subscribed = false;
        od_nrf51_long_write_reset(&long_write);
        od_nrf51_slim_reset();
        advertise();
        break;
    case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
        fatal(sd_ble_gap_sec_params_reply(connection,
              BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL));
        break;
    case BLE_GATTS_EVT_SYS_ATTR_MISSING:
        fatal(sd_ble_gatts_sys_attr_set(connection, NULL, 0u, 0u));
        break;
    case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
        if (event->evt.gatts_evt.params.authorize_request.type ==
            BLE_GATTS_AUTHORIZE_TYPE_WRITE) {
            authorize_write(&event->evt.gatts_evt.params.authorize_request.request.write);
        }
        break;
    case BLE_GATTS_EVT_WRITE: {
        const ble_gatts_evt_write_t *write = &event->evt.gatts_evt.params.write;
        if (write->handle == characteristic.cccd_handle && write->offset == 0u &&
            write->len == 2u && write->op == BLE_GATTS_OP_WRITE_REQ) {
            subscribed = (write->data[0] & BLE_GATT_HVX_NOTIFICATION) != 0u;
            if (!subscribed) {
                od_nrf51_long_write_reset(&long_write);
                od_nrf51_slim_reset();
            }
        }
        break;
    }
    case BLE_EVT_USER_MEM_REQUEST:
        /* NULL deliberately selects S130 NOBUF+AUTH queued-write operation. */
        fatal(sd_ble_user_mem_reply(connection, NULL));
        break;
    case BLE_EVT_USER_MEM_RELEASE:
        od_nrf51_long_write_reset(&long_write);
        break;
    case BLE_GATTS_EVT_TIMEOUT:
        (void)sd_ble_gap_disconnect(connection, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        break;
    case BLE_EVT_TX_COMPLETE:
    default:
        break;
    }
}

bool od_nrf51_ble_process_one(void)
{
    uint8_t event_buffer[OD_NRF51_BLE_EVENT_BUFFER_SIZE] __attribute__((aligned(4)));
    uint16_t length = sizeof(event_buffer);
    uint32_t result = sd_ble_evt_get(event_buffer, &length);

    if (result == NRF_ERROR_NOT_FOUND) {
        uint32_t soc_event;
        result = sd_evt_get(&soc_event);
        if (result == NRF_SUCCESS) {
            return true;
        }
        if (result == NRF_ERROR_NOT_FOUND) {
            return false;
        }
        fatal(result);
        return false;
    }
    if (result != NRF_SUCCESS) {
        fatal(result);
        return false;
    }
    handle_event((const ble_evt_t *)event_buffer);
    return true;
}

void od_nrf51_ble_flush(void)
{
    const uint8_t *frame = NULL;
    uint8_t length = 0u;

    while (healthy && subscribed && connection != BLE_CONN_HANDLE_INVALID &&
           od_nrf51_slim_tx_peek(&frame, &length)) {
        uint16_t sent = length;
        ble_gatts_hvx_params_t params;
        uint32_t result;

        memset(&params, 0, sizeof(params));
        params.handle = characteristic.value_handle;
        params.type = BLE_GATT_HVX_NOTIFICATION;
        params.p_len = &sent;
        params.p_data = (uint8_t *)frame;
        result = sd_ble_gatts_hvx(connection, &params);
        if (result == BLE_ERROR_NO_TX_PACKETS || result == NRF_ERROR_BUSY) {
            return;
        }
        if (result != NRF_SUCCESS || sent != length) {
            fatal(result != NRF_SUCCESS ? result : NRF_ERROR_DATA_SIZE);
            return;
        }
        od_nrf51_slim_tx_accepted();
        od_nrf51_slim_tick();
    }
}

bool od_nrf51_ble_healthy(void)
{
    return healthy;
}

void od_nrf51_ble_note_progress(void)
{
    loop_counter = (uint8_t)((loop_counter + 1u) & 0x0Fu);
}

#ifdef OD_NRF51_BLE_HOST_TEST
void od_nrf51_ble_test_reset(uint16_t value_handle, uint16_t cccd_handle)
{
    connection = BLE_CONN_HANDLE_INVALID;
    characteristic.value_handle = value_handle;
    characteristic.cccd_handle = cccd_handle;
    subscribed = false;
    healthy = true;
    reboot_flag = true;
    loop_counter = 0u;
    od_nrf51_boot_error = NRF_SUCCESS;
    od_nrf51_required_ram_start = 0u;
    od_nrf51_long_write_reset(&long_write);
    od_nrf51_slim_reset();
}

void od_nrf51_ble_test_handle(const ble_evt_t *event)
{
    handle_event(event);
}
#endif
