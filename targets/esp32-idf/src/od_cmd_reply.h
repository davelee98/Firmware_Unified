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

/* Drain queued replies before a blocking panel refresh. Call it AFTER the END ack is queued and
 * IMMEDIATELY BEFORE the refresh, on every path that blocks: a full-image refresh and both partial
 * ones (direct and PIPE).
 *
 * The loop task is the queue's only drainer, and a refresh plus its wait occupies that task for
 * seconds -- up to the ~240 s panel bound the watchdog is sized for. An ack still sitting in the
 * queue when the refresh starts therefore does not reach air until the panel is finished, by which
 * time the host has spent its 500 ms tail-flush read, dup-probed three times and aborted a transfer
 * that in fact completed. Only the full-image path drains today (an inline serviceBleTx()); both
 * partial paths go straight into the refresh, which is the bug this closes.
 *
 * BOUNDED, because the peer can vanish mid-refresh and a queue that never drains must not wedge the
 * loop. On the deadline od_txq_flush() reports TIMEOUT and LEAVES THE ENTRIES QUEUED -- they go out
 * after the refresh, late rather than dropped -- so there is nothing here to report and no caller
 * decision to make. Hence void.
 *
 * The bound holds only as far as od_hal_radio.h's MUST NOT BLOCK does. It does on BLE. It does NOT
 * on the ESP32 LAN arm, whose synchronous socket write has no send timeout, so one stalled TLS peer
 * can park the first flush past any deadline this helper sets -- see od_hal_radio.cpp. Unreachable
 * under legacy routing and a cutover blocker, not something a longer deadline here would fix.
 *
 * The deadline is half the host's tail-flush read, so a delayed ack still beats the first PTO probe
 * rather than racing it; at a slow 30 ms connection interval it also covers ~8 notification
 * opportunities against a deepest tail of 3 frames.
 *
 * Inert until the cutover: under legacy routing od_cmd_reply() hands frames to the shipped sender,
 * so the queue is empty and the first flush returns OK. It is wired now so that the cutover flips
 * routing only, with the barrier already in place on all three paths. */
void od_cmd_flush_before_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_REPLY_H */
