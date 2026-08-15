/* od_cmd_reply.h -- the reply adapter every ESP32 handler calls during the migration.
 *
 * WHY AN ADAPTER AND NOT od_reply() DIRECTLY. Converting 78 call sites and switching which egress
 * is live are two different changes with two different risks, and doing them together makes a
 * hardware failure unattributable. So the call sites move first -- gaining an explicit reply
 * context and, crucially, an explicit PLAIN-or-PROTECTED choice -- while these functions still
 * route to the shipped sender. Nothing observable changes until the cutover flips the routing.
 *
 * The classification is the point. The shipped code infers confidentiality from the response
 * bytes, and that inference is the defect: ESP32 reads byte 2 with a PIPE-ACK carve-out, and
 * Nordic read byte 0 and so sealed its own rejection frames, which the host then validated as an
 * ACK for a command the device refused. A response is bytes whose meaning depends on the opcode,
 * so the producer states its intent instead:
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
