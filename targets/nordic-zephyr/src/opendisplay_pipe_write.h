#ifndef OPENDISPLAY_PIPE_WRITE_H
#define OPENDISPLAY_PIPE_WRITE_H

#include <stdbool.h>
#include "od_cmd.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The reply callback carries the dispatch context rather than a connection number: WHERE a reply
 * goes and WHAT capacity it may spend are properties of the frame being answered, not of the link.
 * The single connection this target supports made the old parameter a constant 0. */
typedef void (*opendisplay_pipe_reply_fn)(const od_cmd_ctx_t *ctx, const uint8_t *data,
                                          uint16_t len);

void opendisplay_pipe_write_reset(void);
bool opendisplay_pipe_write_active(void);
void opendisplay_pipe_write_start(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len,
                                  opendisplay_pipe_reply_fn reply);
void opendisplay_pipe_write_data(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len,
                                 opendisplay_pipe_reply_fn reply);
void opendisplay_pipe_write_end(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len,
                                opendisplay_pipe_reply_fn reply);

#ifdef __cplusplus
}
#endif

#endif
