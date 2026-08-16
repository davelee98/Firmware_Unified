/* od_gate.h -- the encryption gate: every od_session result turned into a wire action.
 *
 * Split out of od_dispatch deliberately. The mapping is the part with a security consequence for
 * every branch, it has to be EXHAUSTIVE over three enums totalling 27 members, and it is the part
 * a host test can drive one member at a time. Dispatch's job -- ordering, reservation, producer
 * conflicts -- is a different concern and is tested against a different set of failures.
 *
 * EXHAUSTIVENESS IS ENFORCED BY THE COMPILER, not by review: the switches below carry no `default`
 * label, so -Wswitch (in -Wall, with -Werror) fails the build when a member is added to
 * od_session_auth or od_session_open without a disposition here. That is the ratchet; do not add a
 * default to silence it, because a new result silently inheriting some other branch's wire
 * behaviour is exactly the failure this arrangement exists to prevent.
 *
 * CONTEXT: single loop/main task, like od_txq and od_session. No locks.
 */

#ifndef OD_GATE_H
#define OD_GATE_H

#include "od_cmd.h"
#include "od_session.h"
#include "od_span.h"
#include "od_txq.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the gate concluded, and the plaintext it produced when it let a frame through. */
typedef struct {
    od_frame_outcome_t outcome;
    od_span_t          body;    /* the decrypted payload; valid ONLY when outcome == ACCEPTED */
} od_gate_result_t;

/* Run the 0x0050 handshake and queue its reply.
 *
 * `body` is the bytes after the two command bytes. The reply is ALWAYS plaintext -- both status
 * codes are 0x00 and a sealed handshake could not be read by a client that has no session yet.
 * Spends one unit of `r`.
 *
 * Never returns ACCEPTED: a handshake is answered here and never reaches a handler. */
od_frame_outcome_t od_gate_authenticate(od_tx_reservation_t *r, const od_reply_t *rp,
                                        od_span_t body);

/* Admit or refuse one encrypted frame.
 *
 * `envelope` is the bytes after the two command bytes. `scratch` receives the decrypted
 * [len:1][payload] frame and must be at least OD_SESSION_PLAIN_MAX; the returned body points into
 * it, so it must outlive the dispatch.
 *
 * On any non-ACCEPTED outcome the reply has already been queued (or deliberately withheld) and the
 * caller must not emit a second one. `body` is empty and MUST NOT be read: on a bad tag the
 * scratch holds unverified plaintext, and acting on it is decrypting for the attacker. */
od_gate_result_t od_gate_open(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                              od_span_t envelope, uint8_t *scratch, size_t scratch_cap);

#ifdef __cplusplus
}
#endif

#endif /* OD_GATE_H */
