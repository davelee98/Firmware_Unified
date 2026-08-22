/* od_dispatch_ops.h -- one metadata row per ordinary routed opcode.
 *
 * The dispatcher expands this table into both its route and its response budget. Keeping those
 * two properties in one row makes it impossible to add a multi-reply hook while accidentally
 * leaving the one-unit default budget in place.
 *
 * AUTHENTICATE belongs to od_gate and FIRMWARE_VERSION is the explicit pre-gate discovery path;
 * both remain outside this ordinary-route table and have one-unit assertions in od_dispatch.c.
 */

#ifndef OD_DISPATCH_OPS_H
#define OD_DISPATCH_OPS_H

#include "od_caps.h"

#define OD_DISPATCH_OPCODE_ROWS(X)                                                   \
    X(CMD_REBOOT,              od_cmd_app_reboot,          1u)                       \
    X(CMD_CONFIG_READ,         od_cmd_app_config_read,     1u)                       \
    X(CMD_CONFIG_WRITE,        od_cmd_app_config_write,    1u)                       \
    X(CMD_CONFIG_CHUNK,        od_cmd_app_config_chunk,    1u)                       \
    X(CMD_READ_MSD,            od_cmd_app_read_msd,        1u)                       \
    X(CMD_CONFIG_CLEAR,        od_cmd_app_config_clear,    1u)                       \
    X(CMD_ENTER_DFU,           od_cmd_app_enter_dfu,       1u)                       \
    X(CMD_POWER_OFF,           od_cmd_app_power_off,       1u)                       \
    X(CMD_DEEP_SLEEP,          od_cmd_app_deep_sleep,      1u)                       \
    X(CMD_DIRECT_WRITE_START,  od_xfer_direct_start,       1u)                       \
    X(CMD_DIRECT_WRITE_DATA,   od_xfer_data,               2u)                       \
    X(CMD_DIRECT_WRITE_END,    od_xfer_end,                2u)                       \
    X(CMD_LED_ACTIVATE,        od_cmd_app_led_activate,    1u)                       \
    X(CMD_LED_STOP,            od_cmd_app_led_stop,        1u)                       \
    X(CMD_PARTIAL_WRITE_START, od_xfer_partial_start,      1u)                       \
    X(CMD_BUZZER,              od_cmd_app_buzzer,          1u)                       \
    X(CMD_PIPE_WRITE_START,    od_pipe_start,              1u)                       \
    X(CMD_PIPE_WRITE_DATA,     od_pipe_data,               (OD_CAP_PIPE ? 3u : 1u))  \
    X(CMD_PIPE_WRITE_END,      od_pipe_end,                (OD_CAP_PIPE ? 3u : 1u))  \
    X(CMD_NFC_ENDPOINT,        od_nfc_frame,               1u)

#endif /* OD_DISPATCH_OPS_H */
