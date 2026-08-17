#ifndef OPENDISPLAY_PIPE_H
#define OPENDISPLAY_PIPE_H

#include "sl_bt_api.h"
#include <stdbool.h>
#include <stdint.h>

void opendisplay_pipe_set_characteristic(uint16_t pipe_value_handle);

void opendisplay_pipe_on_connection_closed(void);

void opendisplay_pipe_handle_gatt_event(sl_bt_msg_t *evt);

void opendisplay_pipe_process(void);

bool opendisplay_pipe_notify_enabled(void);
uint8_t opendisplay_pipe_connection(void);
uint16_t opendisplay_pipe_characteristic(void);
uint32_t opendisplay_pipe_connection_tag(void);

/* Boot-time capability result. Boot API errors enter Gecko AppLoader and are never passed here;
 * this setter remains defensive/testable so a lost capability makes DIRECT END fail closed.
 *
 * There is deliberately no MTU seam. The stack bounds every notification at ATT_MTU - 3 and is
 * the only party that cannot be stale about it (od_hal_radio.c). */
void opendisplay_pipe_set_tx_report_available(bool available);

/* Bounded post-acceptance barrier used by DIRECT END before panel refresh. */
bool opendisplay_pipe_wait_tx_idle(uint32_t tag, uint32_t deadline_ms);
void opendisplay_pipe_close_tag(uint32_t tag);

/* Drop target command state plus the shared producer/queue/session state. */
void opendisplay_pipe_reset_transport(void);

#endif
