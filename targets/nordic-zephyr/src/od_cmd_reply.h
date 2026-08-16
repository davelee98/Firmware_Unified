/* od_cmd_reply.h -- the reply entry point every Nordic handler calls.
 *
 * WHY AN ADAPTER, AND WHY IT ROUTES TO THE OLD SENDER FOR NOW. Converting 70 call sites and
 * switching which egress is live are two changes with two different risks, and doing them together
 * makes a hardware failure unattributable -- which is exactly how the ESP32 conversion produced 26
 * inverted verdicts from a single blanket transform. So the call sites move first, gaining an
 * explicit PLAIN-or-PROTECTED choice, while these functions still hand frames to pipe_send().
 * Nothing observable changes until the cutover flips the routing.
 *
 * THE CLASSIFICATION IS THE POINT, and this target is the reason the rule exists. pipe_send()
 * infers confidentiality from the response bytes -- byte 2, with a carve-out for the 7-byte PIPE
 * ACK whose byte 2 is a rolling sequence number. Before that carve-out it read byte 0 and so
 * SEALED its own rejection frames, which py-opendisplay then decrypted and validated as an ACK for
 * a command the device had refused. A response is bytes whose meaning depends on the opcode, so
 * the producer states its intent instead:
 *
 *   od_cmd_reply_plain()  control and error frames -- auth-required, decrypt failure, every hard
 *                         NACK, AUTHENTICATE, FIRMWARE_VERSION. Never sealed, whatever the session.
 *   od_cmd_reply()        application responses, INCLUDING all PIPE ACKs.
 *
 * No handler may choose by looking at its own payload.
 *
 * RETURNS A STATUS, and it is not decorative: after a non-OK the caller MUST NOT emit another wire
 * response. od_reply() can substitute a plaintext hard NACK for a frame it could not seal, and a
 * multi-reply path that ignored that would queue a contradictory success behind it.
 */

#ifndef OD_CMD_REPLY_H
#define OD_CMD_REPLY_H

#include "od_cmd.h"
#include "od_txq.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len);
od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_REPLY_H */
