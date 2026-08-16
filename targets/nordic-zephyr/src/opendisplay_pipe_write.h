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

/* THEY RETURN VERDICTS. A void API forced the caller into an unconditional OD_CMD_OK, so a PIPE
 * frame answered with a hard NACK was reported to the dispatcher as an accepted command -- and the
 * verdict is what decides whether a frame stamps the session's activity clock. Traffic the device
 * refused kept the link's clock running.
 *
 * OD_CMD_OK means this frame was ACCEPTED and answered as such; OD_CMD_NACK means it was refused,
 * whether the refusal was a NACK frame, a reply od_reply() had to substitute one for, or a silent
 * discard. Silence is still refusal: nothing was accepted and nothing changed. */
void opendisplay_pipe_write_reset(void);
bool opendisplay_pipe_write_active(void);
od_cmd_result_t opendisplay_pipe_write_start(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len);
od_cmd_result_t opendisplay_pipe_write_data(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len);
od_cmd_result_t opendisplay_pipe_write_end(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
