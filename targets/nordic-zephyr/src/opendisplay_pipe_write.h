#ifndef OPENDISPLAY_PIPE_WRITE_H
#define OPENDISPLAY_PIPE_WRITE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*opendisplay_pipe_reply_fn)(uint8_t connection, const uint8_t *data, uint16_t len);

void opendisplay_pipe_write_reset(void);
bool opendisplay_pipe_write_active(void);
void opendisplay_pipe_write_start(uint8_t connection, const uint8_t *payload, uint16_t payload_len,
                                  opendisplay_pipe_reply_fn reply);
void opendisplay_pipe_write_data(uint8_t connection, const uint8_t *payload, uint16_t payload_len,
                                 opendisplay_pipe_reply_fn reply);
void opendisplay_pipe_write_end(uint8_t connection, const uint8_t *payload, uint16_t payload_len,
                                opendisplay_pipe_reply_fn reply);

#ifdef __cplusplus
}
#endif

#endif
