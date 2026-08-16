#ifndef OPENDISPLAY_PIPE_WRITE_H
#define OPENDISPLAY_PIPE_WRITE_H

#include <stdbool.h>
#include "od_cmd.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NO REPLY CALLBACK. It used to be passed in, which meant every PIPE reply bypassed the reply
 * adapter and kept the shipped byte-2 inference while the rest of the target moved -- a split
 * egress on the HIGHEST-rate path, and the one that exhausts the TX pool. A single function
 * pointer also cannot express the choice that matters: a PIPE data ACK is protected and a PIPE
 * NACK is plaintext. The module calls od_cmd_reply{,_plain}() directly and states which. */

void opendisplay_pipe_write_reset(void);
bool opendisplay_pipe_write_active(void);
void opendisplay_pipe_write_start(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len);
void opendisplay_pipe_write_data(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len);
void opendisplay_pipe_write_end(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
