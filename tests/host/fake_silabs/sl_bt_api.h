#ifndef OD_TEST_FAKE_SILABS_BT_API_H
#define OD_TEST_FAKE_SILABS_BT_API_H

#include "sl_status.h"

#include <stdint.h>

#define sl_bt_evt_connection_opened_id                    1u
#define sl_bt_evt_connection_closed_id                    2u
#define sl_bt_evt_gatt_server_attribute_value_id          3u
#define sl_bt_evt_gatt_server_characteristic_status_id    4u
#define sl_bt_evt_gatt_mtu_exchanged_id                   5u
#define SL_BT_MSG_ID(header_)                              (header_)

#define sl_bt_gatt_write_request              1u
#define sl_bt_gatt_write_command              2u
#define sl_bt_gatt_server_client_config       1u
#define sl_bt_gatt_server_notification        1u

#define SL_BT_RESOURCE_CONNECTION_TX_FLAGS_ERROR_PACKET_OVERFLOW 0x0001u
#define SL_BT_RESOURCE_CONNECTION_TX_FLAGS_ERROR_CORRUPT         0x0002u

typedef struct { uint8_t connection; } sl_bt_evt_connection_opened_t;
typedef struct { uint16_t reason; } sl_bt_evt_connection_closed_t;

typedef struct {
    uint8_t len;
    uint8_t data[263];
} od_test_bt_value_t;

typedef struct {
    uint16_t attribute;
    uint16_t offset;
    uint8_t att_opcode;
    od_test_bt_value_t value;
} sl_bt_evt_gatt_server_attribute_value_t;

typedef struct {
    uint16_t characteristic;
    uint8_t status_flags;
    uint16_t client_config_flags;
} sl_bt_evt_gatt_server_characteristic_status_t;

typedef struct {
    uint8_t connection;
    uint16_t mtu;
} sl_bt_evt_gatt_mtu_exchanged_t;

typedef struct {
    uint32_t header;
    union {
        sl_bt_evt_connection_opened_t evt_connection_opened;
        sl_bt_evt_connection_closed_t evt_connection_closed;
        sl_bt_evt_gatt_server_attribute_value_t evt_gatt_server_attribute_value;
        sl_bt_evt_gatt_server_characteristic_status_t evt_gatt_server_characteristic_status;
        sl_bt_evt_gatt_mtu_exchanged_t evt_gatt_mtu_exchanged;
    } data;
} sl_bt_msg_t;

void sl_bt_run(void);
sl_status_t sl_bt_connection_close(uint8_t connection);
sl_status_t sl_bt_gatt_server_send_notification(uint8_t connection, uint16_t characteristic,
                                                 uint16_t value_len, const uint8_t *value);
sl_status_t sl_bt_resource_get_connection_tx_status(uint8_t connection, uint16_t *flags,
                                                     uint16_t *packet_count,
                                                     uint32_t *data_len);

#endif /* OD_TEST_FAKE_SILABS_BT_API_H */
