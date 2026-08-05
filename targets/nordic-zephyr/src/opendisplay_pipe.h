#ifndef OPENDISPLAY_PIPE_H
#define OPENDISPLAY_PIPE_H

#include <stdbool.h>
#include <stdint.h>

void opendisplay_pipe_on_write(const uint8_t *data, uint16_t len, bool write_cmd);
void opendisplay_pipe_on_connection_closed(void);
void opendisplay_pipe_on_notify_changed(bool enabled);
void opendisplay_pipe_process(void);

#endif
