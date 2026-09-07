#ifndef OD_TEST_FAKE_S130_H
#define OD_TEST_FAKE_S130_H

#include <stdint.h>

#define NRF_SUCCESS 0u
#define NRF_ERROR_BUSY 1u
#define NRF_ERROR_NOT_FOUND 2u
#define NRF_ERROR_DATA_SIZE 3u

#define BLE_ERROR_NO_TX_PACKETS 4u
#define BLE_CONN_HANDLE_INVALID 0xFFFFu
#define BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION 0x13u

#define BLE_GATT_STATUS_SUCCESS 0u
#define BLE_GATT_STATUS_ATTERR_APP_BEGIN 0x80u
#define BLE_GATT_STATUS_ATTERR_CPS_CCCD_CONFIG_ERROR 0xFDu
#define BLE_GATT_STATUS_ATTERR_INVALID_ATT_VAL_LENGTH 0x0Du
#define BLE_GATT_STATUS_ATTERR_REQUEST_NOT_SUPPORTED 0x06u
#define BLE_GATT_HVX_NOTIFICATION 1u

#define BLE_GATTS_AUTHORIZE_TYPE_WRITE 2u
#define BLE_GATTS_OP_WRITE_REQ 1u
#define BLE_GATTS_OP_WRITE_CMD 2u
#define BLE_GATTS_OP_PREP_WRITE_REQ 3u
#define BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL 4u
#define BLE_GATTS_OP_EXEC_WRITE_REQ_NOW 5u

#define BLE_GAP_EVT_CONNECTED 0x10u
#define BLE_GAP_EVT_DISCONNECTED 0x11u
#define BLE_GAP_EVT_SEC_PARAMS_REQUEST 0x12u
#define BLE_GATTS_EVT_SYS_ATTR_MISSING 0x20u
#define BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST 0x21u
#define BLE_GATTS_EVT_WRITE 0x22u
#define BLE_GATTS_EVT_TIMEOUT 0x23u
#define BLE_EVT_USER_MEM_REQUEST 0x30u
#define BLE_EVT_USER_MEM_RELEASE 0x31u
#define BLE_EVT_TX_COMPLETE 0x32u

#define BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP 0x05u

typedef struct {
    uint16_t value_handle;
    uint16_t user_desc_handle;
    uint16_t cccd_handle;
    uint16_t sccd_handle;
} ble_gatts_char_handles_t;

typedef struct {
    uint16_t handle;
    uint8_t op;
    uint8_t auth_required;
    uint16_t offset;
    uint16_t len;
    uint8_t data[20];
} ble_gatts_evt_write_t;

typedef struct {
    uint16_t gatt_status;
    uint8_t update;
    uint16_t offset;
    uint16_t len;
    const uint8_t *p_data;
} ble_gatts_authorize_params_t;

typedef struct {
    uint8_t type;
    union {
        ble_gatts_authorize_params_t read;
        ble_gatts_authorize_params_t write;
    } params;
} ble_gatts_rw_authorize_reply_params_t;

typedef struct {
    uint16_t handle;
    uint8_t type;
    uint16_t offset;
    uint16_t *p_len;
    uint8_t *p_data;
} ble_gatts_hvx_params_t;

typedef struct {
    uint16_t evt_id;
    uint16_t evt_len;
} ble_evt_hdr_t;

typedef struct {
    uint8_t type;
    union {
        ble_gatts_evt_write_t write;
    } request;
} ble_gatts_evt_rw_authorize_request_t;

typedef struct {
    uint16_t conn_handle;
    union {
        ble_gatts_evt_write_t write;
        ble_gatts_evt_rw_authorize_request_t authorize_request;
    } params;
} ble_gatts_evt_t;

typedef struct {
    uint16_t conn_handle;
} ble_gap_evt_t;

typedef struct {
    ble_evt_hdr_t header;
    union {
        ble_gap_evt_t gap_evt;
        ble_gatts_evt_t gatts_evt;
    } evt;
} ble_evt_t;

uint32_t sd_temp_get(int32_t *temperature);
uint32_t sd_ble_gatts_rw_authorize_reply(
    uint16_t connection, const ble_gatts_rw_authorize_reply_params_t *reply);
uint32_t sd_ble_gap_sec_params_reply(uint16_t connection, uint8_t status,
                                    const void *security, const void *keys);
uint32_t sd_ble_gatts_sys_attr_set(uint16_t connection, const uint8_t *data,
                                  uint16_t length, uint32_t flags);
uint32_t sd_ble_user_mem_reply(uint16_t connection, const void *block);
uint32_t sd_ble_gap_disconnect(uint16_t connection, uint8_t reason);
uint32_t sd_ble_evt_get(uint8_t *buffer, uint16_t *length);
uint32_t sd_evt_get(uint32_t *event);
uint32_t sd_ble_gatts_hvx(uint16_t connection, ble_gatts_hvx_params_t *params);

#endif
