/* od_reply.c -- see od_reply.h for why confidentiality is chosen by the call, not by the bytes. */

#include "od_reply.h"

#include "od_session.h"
#include "od_session_app.h"

#include <string.h>

/* The one substitution this file makes. When a response cannot be sealed, sending it in the clear
 * would hand the host payload it expected protected, and sending nothing would hang a client
 * waiting on an ack. A three-byte hard NACK does neither: {0xFF, cmd, 0x00} echoes the opcode so
 * the client can match it to its request, and 0xFF at byte 0 is what every target already routes
 * plaintext. */
static od_txq_status_t queue_hard_nack(od_tx_reservation_t *r, const od_reply_t *rp,
                                       uint8_t cmd_low, od_txq_status_t why)
{
    uint8_t nack[3];
    od_txq_status_t rc;

    nack[0] = RESP_NACK;
    nack[1] = cmd_low;
    nack[2] = 0x00u;
    rc = od_txq_commit(r, rp, nack, sizeof nack);
    /* The substitution's own failure wins: the caller needs to know nothing was queued at all,
     * which is a different situation from "your response became a NACK". */
    return (rc == OD_TXQ_OK) ? why : rc;
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    return od_txq_commit(r, rp, frame, len);
}

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    uint8_t sealed[OD_SESSION_SEALED_MAX];
    uint16_t sealed_len = 0u;
    struct od_session_report report;
    struct od_session *s;
    enum od_session_seal rc;

    if (r == NULL || rp == NULL || frame == NULL || len == 0u) {
        return OD_TXQ_INVARIANT;
    }
    /* TLS already protects this frame; a second envelope would leave the host unable to decode
     * either layer. SECTION 9 rule 4. */
    if (rp->origin == OD_ORIGIN_LAN_TLS) {
        return od_txq_commit(r, rp, frame, len);
    }
    /* No security configured is ordinary unencrypted operation, not a failure to protect. */
    if (!od_session_security_enabled(od_session_app_security())) {
        return od_txq_commit(r, rp, frame, len);
    }

    s = od_session_app_state();
    rc = od_session_seal(s, od_span_make(frame, len), sealed, sizeof sealed, &sealed_len,
                         od_session_app_now_ms(), &report);
    od_session_app_report(OD_SESSION_APP_SEAL, (int)rc, (uint16_t)(((uint16_t)frame[0] << 8) |
                          (len > 1u ? frame[1] : 0u)), &report);

    switch (rc) {
    case OD_SESSION_SEAL_OK:
        return od_txq_commit(r, rp, sealed, sealed_len);

    case OD_SESSION_SEAL_TOO_SHORT:
        /* Under two bytes there is no opcode to echo, so no honest NACK can be built. Emit
         * nothing and leave the unit for the caller's mandatory release. */
        return OD_TXQ_INVARIANT;

    case OD_SESSION_SEAL_TOO_LONG:
    case OD_SESSION_SEAL_NO_ROOM:
        /* A producer built something no frame can carry. Detected before a nonce was drawn, so
         * nothing was spent but the reply itself. */
        return queue_hard_nack(r, rp, frame[1], OD_TXQ_TOO_LARGE);

    case OD_SESSION_SEAL_NO_SESSION:
        /* Security is on and this response was meant to be protected, so it must NOT go out in
         * the clear -- that would leak to an unauthenticated peer exactly the payload the session
         * exists to hide. */
        return queue_hard_nack(r, rp, frame[1], OD_TXQ_SEAL_FAILED);

    case OD_SESSION_SEAL_CRYPTO_ERROR:
        /* May have spent one counter value. Never resealed and never retried: a second attempt
         * would spend another, and the counter is what makes the nonce unique. */
        return queue_hard_nack(r, rp, frame[1], OD_TXQ_SEAL_FAILED);

    case OD_SESSION_SEAL_COUNTER_EXHAUSTED:
        /* The next nonce would wrap to zero and repeat one already used under this key, which is
         * total CCM failure. Force re-authentication rather than emit it. */
        od_session_clear(s);
        return queue_hard_nack(r, rp, frame[1], OD_TXQ_SEAL_FAILED);
    }
    /* NO DEFAULT LABEL, deliberately -- the same -Wswitch ratchet od_gate.c relies on. A default
     * here would let a new od_session_seal result compile straight into "hard NACK" whatever its
     * required disposition is, which is how a new failure mode silently inherits another's wire
     * behaviour. Unreachable. */
    return queue_hard_nack(r, rp, frame[1], OD_TXQ_SEAL_FAILED);
}
