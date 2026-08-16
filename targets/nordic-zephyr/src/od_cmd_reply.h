/* od_cmd_reply.h -- the reply entry point every Nordic handler calls.
 *
 * A thin named seam over od_reply(), kept because the classification below is a per-call-site
 * decision that has to be legible at the site.
 *
 * THE CLASSIFICATION IS THE POINT, and this target is the reason the rule exists. The sender this replaces
 * inferred confidentiality from the response bytes -- byte 2, with a carve-out for the 7-byte PIPE
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
