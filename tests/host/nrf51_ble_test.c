#include "fake_s130.h"
#include "od_nrf51_ble.h"
#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"
#include "opendisplay_protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void od_nrf51_ble_test_reset(uint16_t value_handle, uint16_t cccd_handle);
void od_nrf51_ble_test_handle(const ble_evt_t *event);

static int failures;
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static uint32_t authorize_result;
static unsigned authorize_calls;
static ble_gatts_rw_authorize_reply_params_t last_authorize;
static uint32_t hvx_result;
static unsigned hvx_calls;
static uint8_t last_hvx[OD_NRF51_NOTIFY_MAX];
static uint16_t last_hvx_length;
static unsigned sec_reply_calls;
static unsigned sys_attr_calls;
static unsigned user_mem_calls;
static unsigned disconnect_calls;
static bool queued_ble_valid;
static bool queued_soc_valid;
static ble_evt_t queued_ble;

uint32_t sd_temp_get(int32_t *temperature)
{
    *temperature = 4;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_rw_authorize_reply(
    uint16_t connection, const ble_gatts_rw_authorize_reply_params_t *reply)
{
    (void)connection;
    authorize_calls++;
    last_authorize = *reply;
    return authorize_result;
}

uint32_t sd_ble_gap_sec_params_reply(uint16_t connection, uint8_t status,
                                    const void *security, const void *keys)
{
    (void)connection;
    (void)security;
    (void)keys;
    CHECK(status == BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP);
    sec_reply_calls++;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_sys_attr_set(uint16_t connection, const uint8_t *data,
                                  uint16_t length, uint32_t flags)
{
    (void)connection;
    CHECK(data == NULL && length == 0u && flags == 0u);
    sys_attr_calls++;
    return NRF_SUCCESS;
}

uint32_t sd_ble_user_mem_reply(uint16_t connection, const void *block)
{
    (void)connection;
    CHECK(block == NULL);
    user_mem_calls++;
    return NRF_SUCCESS;
}

uint32_t sd_ble_gap_disconnect(uint16_t connection, uint8_t reason)
{
    (void)connection;
    CHECK(reason == BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    disconnect_calls++;
    return NRF_SUCCESS;
}

uint32_t sd_ble_evt_get(uint8_t *buffer, uint16_t *length)
{
    if (!queued_ble_valid) {
        return NRF_ERROR_NOT_FOUND;
    }
    if (*length < sizeof(queued_ble)) {
        *length = sizeof(queued_ble);
        return NRF_ERROR_DATA_SIZE;
    }
    memcpy(buffer, &queued_ble, sizeof(queued_ble));
    *length = sizeof(queued_ble);
    queued_ble_valid = false;
    return NRF_SUCCESS;
}

uint32_t sd_evt_get(uint32_t *event)
{
    if (queued_soc_valid) {
        *event = 1u;
        queued_soc_valid = false;
        return NRF_SUCCESS;
    }
    return NRF_ERROR_NOT_FOUND;
}

uint32_t sd_ble_gatts_hvx(uint16_t connection, ble_gatts_hvx_params_t *params)
{
    (void)connection;
    hvx_calls++;
    if (hvx_result == NRF_SUCCESS) {
        CHECK(*params->p_len <= sizeof(last_hvx));
        last_hvx_length = *params->p_len;
        memcpy(last_hvx, params->p_data, last_hvx_length);
    }
    return hvx_result;
}

uint32_t od_nrf51_now_ms(void)
{
    return 0u;
}

bool od_nrf51_panel_begin(void)
{
    return true;
}

bool od_nrf51_panel_write(const uint8_t *data, uint16_t length)
{
    (void)data;
    (void)length;
    return true;
}

bool od_nrf51_panel_refresh_start(void)
{
    return true;
}

int od_nrf51_panel_poll(void)
{
    return 0;
}

void od_nrf51_panel_abort(void)
{
}

static void event(uint16_t id)
{
    ble_evt_t value;

    memset(&value, 0, sizeof(value));
    value.header.evt_id = id;
    value.evt.gap_evt.conn_handle = 7u;
    od_nrf51_ble_test_handle(&value);
}

static void ccc_write(bool enable)
{
    ble_evt_t value;
    ble_gatts_evt_write_t *write;

    memset(&value, 0, sizeof(value));
    value.header.evt_id = BLE_GATTS_EVT_WRITE;
    write = &value.evt.gatts_evt.params.write;
    write->handle = 12u;
    write->op = BLE_GATTS_OP_WRITE_REQ;
    write->len = 2u;
    write->data[0] = enable ? BLE_GATT_HVX_NOTIFICATION : 0u;
    od_nrf51_ble_test_handle(&value);
}

static void authorize(uint8_t op, uint16_t offset, const uint8_t *data, uint16_t length)
{
    ble_evt_t value;
    ble_gatts_evt_write_t *write;

    CHECK(length <= sizeof(write->data));
    memset(&value, 0, sizeof(value));
    value.header.evt_id = BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST;
    value.evt.gatts_evt.params.authorize_request.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
    write = &value.evt.gatts_evt.params.authorize_request.request.write;
    write->handle = 11u;
    write->op = op;
    write->offset = offset;
    write->len = length;
    if (length != 0u) {
        memcpy(write->data, data, length);
    }
    od_nrf51_ble_test_handle(&value);
}

static void reset_fakes(void)
{
    authorize_result = NRF_SUCCESS;
    authorize_calls = 0u;
    memset(&last_authorize, 0, sizeof(last_authorize));
    hvx_result = NRF_SUCCESS;
    hvx_calls = 0u;
    memset(last_hvx, 0, sizeof(last_hvx));
    last_hvx_length = 0u;
    sec_reply_calls = 0u;
    sys_attr_calls = 0u;
    user_mem_calls = 0u;
    disconnect_calls = 0u;
    queued_ble_valid = false;
    queued_soc_valid = false;
    memset(&queued_ble, 0, sizeof(queued_ble));
    od_nrf51_ble_test_reset(11u, 12u);
}

static void connect_and_subscribe(void)
{
    event(BLE_GAP_EVT_CONNECTED);
    ccc_write(true);
}

static void test_control_events(void)
{
    reset_fakes();
    event(BLE_GAP_EVT_CONNECTED);
    event(BLE_GAP_EVT_SEC_PARAMS_REQUEST);
    event(BLE_GATTS_EVT_SYS_ATTR_MISSING);
    event(BLE_EVT_USER_MEM_REQUEST);
    event(BLE_GATTS_EVT_TIMEOUT);
    CHECK(sec_reply_calls == 1u);
    CHECK(sys_attr_calls == 1u);
    CHECK(user_mem_calls == 1u);
    CHECK(disconnect_calls == 1u);

    event(BLE_GAP_EVT_DISCONNECTED);
    event(BLE_GAP_EVT_CONNECTED);
    event(BLE_GATTS_EVT_SYS_ATTR_MISSING);
    ccc_write(true);
    CHECK(sys_attr_calls == 2u);
}

static void test_event_pump(void)
{
    reset_fakes();
    queued_ble.header.evt_id = BLE_GATTS_EVT_SYS_ATTR_MISSING;
    queued_ble_valid = true;
    CHECK(od_nrf51_ble_process_one());
    CHECK(sys_attr_calls == 1u);
    queued_soc_valid = true;
    CHECK(od_nrf51_ble_process_one());
    CHECK(!od_nrf51_ble_process_one());
}

static void test_ccc_and_backpressure(void)
{
    static const uint8_t version[] = { 0x00u, 0x43u };

    reset_fakes();
    event(BLE_GAP_EVT_CONNECTED);
    authorize(BLE_GATTS_OP_WRITE_REQ, 0u, version, sizeof(version));
    CHECK(last_authorize.params.write.gatt_status ==
          BLE_GATT_STATUS_ATTERR_CPS_CCCD_CONFIG_ERROR);
    CHECK(od_nrf51_slim_admission_free());

    ccc_write(true);
    authorize(BLE_GATTS_OP_WRITE_REQ, 0u, version, sizeof(version));
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_SUCCESS);
    CHECK(last_authorize.params.write.update == 1u);
    CHECK(!od_nrf51_slim_admission_free());

    hvx_result = BLE_ERROR_NO_TX_PACKETS;
    od_nrf51_ble_flush();
    CHECK(hvx_calls == 1u);
    CHECK(!od_nrf51_slim_admission_free());
    event(BLE_EVT_TX_COMPLETE);
    hvx_result = NRF_SUCCESS;
    od_nrf51_ble_flush();
    CHECK(hvx_calls == 2u);
    CHECK(last_hvx_length == 14u);
    CHECK(last_hvx[0] == RESP_ACK && last_hvx[1] == RESP_FIRMWARE_VERSION);
    CHECK(od_nrf51_slim_admission_free());

    authorize(BLE_GATTS_OP_WRITE_REQ, 0u, version, sizeof(version));
    CHECK(!od_nrf51_slim_admission_free());
    event(BLE_GAP_EVT_DISCONNECTED);
    CHECK(od_nrf51_slim_admission_free());

    event(BLE_GAP_EVT_CONNECTED);
    ccc_write(true);
    authorize(BLE_GATTS_OP_WRITE_REQ, 0u, version, sizeof(version));
    CHECK(!od_nrf51_slim_admission_free());
    ccc_write(false);
    CHECK(od_nrf51_slim_admission_free());
}

static void test_busy_and_long_write(void)
{
    static const uint8_t config[] = { 0x00u, 0x40u };
    static const uint8_t version_hi[] = { 0x00u };
    static const uint8_t version_lo[] = { 0x43u };

    reset_fakes();
    connect_and_subscribe();
    authorize(BLE_GATTS_OP_WRITE_REQ, 0u, config, sizeof(config));
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_SUCCESS);
    authorize(BLE_GATTS_OP_WRITE_REQ, 0u, version_hi, sizeof(version_hi));
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_ATTERR_APP_BEGIN);
    CHECK(!od_nrf51_slim_admission_free());
    od_nrf51_ble_flush();
    CHECK(od_nrf51_slim_admission_free());

    authorize(BLE_GATTS_OP_PREP_WRITE_REQ, 0u, version_hi, sizeof(version_hi));
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_SUCCESS);
    CHECK(last_authorize.params.write.update == 0u);
    event(BLE_EVT_USER_MEM_RELEASE);
    authorize(BLE_GATTS_OP_EXEC_WRITE_REQ_NOW, 0u, NULL, 0u);
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_ATTERR_APP_BEGIN);

    authorize(BLE_GATTS_OP_PREP_WRITE_REQ, 0u, version_hi, sizeof(version_hi));
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_SUCCESS);
    authorize(BLE_GATTS_OP_PREP_WRITE_REQ, 1u, version_lo, sizeof(version_lo));
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_SUCCESS);
    authorize(BLE_GATTS_OP_EXEC_WRITE_REQ_NOW, 0u, NULL, 0u);
    CHECK(last_authorize.params.write.gatt_status == BLE_GATT_STATUS_SUCCESS);
    od_nrf51_ble_flush();
    CHECK(last_hvx_length == 14u);
    CHECK(last_hvx[1] == RESP_FIRMWARE_VERSION);
}

static void test_authorize_retry_bound(void)
{
    static const uint8_t version[] = { 0x00u, 0x43u };

    reset_fakes();
    connect_and_subscribe();
    authorize_result = NRF_ERROR_BUSY;
    authorize(BLE_GATTS_OP_WRITE_REQ, 0u, version, sizeof(version));
    CHECK(authorize_calls == OD_NRF51_AUTH_REPLY_ATTEMPTS);
    CHECK(!od_nrf51_ble_healthy());
}

static void test_msd_liveness(void)
{
    uint8_t first[16];
    uint8_t second[16];
    uint8_t connected[16];

    reset_fakes();
    od_nrf51_copy_msd(first);
    od_nrf51_ble_note_progress();
    od_nrf51_copy_msd(second);
    CHECK(first[0] == 0x46u && first[1] == 0x24u);
    for (uint8_t index = 2u; index < 13u; ++index) {
        CHECK(first[index] == 0u);
    }
    CHECK(first[13] == 82u);
    CHECK(first[14] == 0u);
    CHECK((first[15] & 0x02u) != 0u);
    CHECK((first[15] >> 4) == 0u);
    CHECK((second[15] >> 4) == 1u);
    event(BLE_GAP_EVT_CONNECTED);
    od_nrf51_ble_note_progress();
    od_nrf51_copy_msd(connected);
    CHECK((connected[15] & 0x02u) == 0u);
    CHECK((connected[15] >> 4) == 2u);
}

int main(void)
{
    test_control_events();
    test_event_pump();
    test_ccc_and_backpressure();
    test_busy_and_long_write();
    test_authorize_retry_bound();
    test_msd_liveness();
    return failures == 0 ? 0 : 1;
}
