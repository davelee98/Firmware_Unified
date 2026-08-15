/* od_dispatch.h -- one command path: validate, reserve, gate, dispatch.
 *
 * THE ORDERING IS THE DESIGN. Everything else here is bookkeeping:
 *
 *   structural validation -> tag liveness -> producer conflict -> response budget
 *     -> RESERVE -> auth/decrypt gate -> handler
 *
 * Reservation precedes the GATE, not merely the handler, because the gate itself answers
 * [00][cmd][FE] and [00][cmd][FF] and so needs a slot of its own. It precedes DECRYPT because
 * decrypt advances the replay window: a frame deferred after decrypting is replayed when it is
 * re-dispatched, and the window will refuse it the second time. That is why OD_FRAME_DEFERRED is
 * returnable ONLY before decrypt, and why the producer-conflict check is pre-decrypt too.
 *
 * The budget is keyed on the OUTER opcode, which is not yet authenticated. That is safe: it
 * reserves at most three slots and mutates nothing, and the same bytes are verified as CCM AAD a
 * moment later, so a lie about the opcode cannot survive the tag check.
 *
 * CONTEXT: single loop/main task. od_dispatch_frame() does NOT promise never to block -- an END
 * blocks through a panel refresh of up to 60 s inside a target handler. It is bounded by the
 * watchdog, not by this API.
 */

#ifndef OD_DISPATCH_H
#define OD_DISPATCH_H

#include "od_cmd.h"
#include "od_span.h"
#include "od_txq.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch one complete inbound frame: [cmd:2][body], where body is an envelope when the frame is
 * encrypted. Answers the wire itself on every path that does not reach a handler.
 *
 * EVERY CALLER MUST THEN CALL od_core_frame_done() with the returned outcome -- BLE drain and LAN
 * entry alike. It is stated here rather than in a doc because the LAN path omitting it is a silent
 * policy regression: an active LAN client's idle clock stops and it is disconnected mid-session.
 *
 * OD_FRAME_DEFERRED means the frame was NOT consumed and must be re-offered later, unchanged. It
 * is the only outcome for which that is true. */
od_frame_outcome_t od_dispatch_frame(const od_reply_t *rp, od_span_t frame);

/* IMPLEMENTED BY THE TARGET. Runs one command that has passed the gate; `body` is plaintext.
 *
 * C8 SCOPE NOTE: the plan names one seam per opcode (od_cmd_led_activate, od_cmd_config_read, ...)
 * as scaffolding with a shrink schedule. This is a single seam instead, so the shared ordering can
 * land and be tested against the targets' EXISTING handler switches rather than against a
 * simultaneous rewrite of every handler signature. The per-opcode split belongs to C11's shrink,
 * where the transfer subsystems move behind their own headers anyway.
 *
 * The handler must complete: dispatch has already resolved capacity and producer conflicts, so
 * there is nothing left for a handler to defer on. It spends units from `r` via od_reply(). */
od_cmd_result_t od_cmd_dispatch(const od_reply_t *rp, od_tx_reservation_t *r,
                                uint16_t cmd, od_span_t body);

/* IMPLEMENTED BY THE TARGET. True when this opcode would MUTATE stored configuration -- the set a
 * live CONFIG_READ must exclude, because the producer reads the same scratch that a write
 * reloads through. Target-side because the opcode set differs per target (LAN-only opcodes, NFC
 * on Nordic, PIPE absent on Silabs). */
bool od_cmd_mutates_config(uint16_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* OD_DISPATCH_H */
