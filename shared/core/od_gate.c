/* od_gate.c -- see od_gate.h for why the switches below have no default label. */

#include "od_gate.h"

#include "od_log.h"
#include "od_nonce_window.h"
#include "od_reply.h"
#include "od_session_app.h"

#include <string.h>

/* [0x00][cmd_low][status]. The three-byte control frame both refusal paths answer with. Always
 * plaintext: a client whose session just failed cannot read a sealed one, which is the whole
 * reason py-opendisplay checks raw[2] on undecrypted frames only. */
static void queue_status(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd, uint8_t status)
{
    uint8_t frame[3];

    frame[0] = RESP_ACK;
    frame[1] = (uint8_t)(cmd & 0xFFu);
    frame[2] = status;
    (void)od_reply_plain(r, rp, frame, sizeof frame);
}

od_frame_outcome_t od_gate_authenticate(od_tx_reservation_t *r, const od_reply_t *rp,
                                        od_span_t body)
{
    uint8_t device_id[OD_SESSION_DEVICE_ID_LEN];
    uint8_t rsp[OD_SESSION_REPLY_MAX];
    uint16_t rsp_len = 0u;
    struct od_session_report report;
    enum od_session_auth rc;

    od_session_app_device_id(device_id);
    rc = od_session_authenticate(od_session_app_state(), od_session_app_security(), device_id,
                                 body, od_session_app_now_ms(),
                                 rsp, sizeof rsp, &rsp_len, &report);
    od_session_app_report(OD_SESSION_APP_AUTH, (int)rc, CMD_AUTHENTICATE, &report);

    switch (rc) {
    case OD_SESSION_AUTH_CHALLENGE:
        (void)od_reply_plain(r, rp, rsp, rsp_len);
        return OD_FRAME_AUTH_CONTROL;

    case OD_SESSION_AUTH_ESTABLISHED:
        /* Resets the auth-abuse run at the target, but still does NOT stamp activity: a peer that
         * can authenticate has not yet done any work worth holding the exclusive link for. */
        (void)od_reply_plain(r, rp, rsp, rsp_len);
        return OD_FRAME_AUTH_ESTABLISHED;

    case OD_SESSION_AUTH_REJECTED:
    case OD_SESSION_AUTH_RATE_LIMITED:
    case OD_SESSION_AUTH_NOT_CONFIGURED:
    case OD_SESSION_AUTH_MALFORMED:
    case OD_SESSION_AUTH_EXPIRED:
    case OD_SESSION_AUTH_CRYPTO_ERROR:
        /* The core always builds a well-formed [00][50][status] for these, so the bytes are its
         * to choose, not ours to invent. */
        (void)od_reply_plain(r, rp, rsp, rsp_len);
        return OD_FRAME_AUTH_CONTROL;

    case OD_SESSION_AUTH_NO_ROOM:
    case OD_SESSION_AUTH_BAD_ARGUMENT:
        /* INVARIANT FAILURES: this caller always passes OD_SESSION_REPLY_MAX, a non-null device id
         * and a non-null length, so neither is reachable. Synthesised rather than trusted, because
         * on these two the core may not have written a reply at all -- and the frame must not be
         * treated as established or as activity. */
        queue_status(r, rp, CMD_AUTHENTICATE, AUTH_STATUS_ERROR);
        return OD_FRAME_AUTH_CONTROL;
    }
    /* No default, so -Wswitch fails the build on a new member. Unreachable. */
    queue_status(r, rp, CMD_AUTHENTICATE, AUTH_STATUS_ERROR);
    return OD_FRAME_AUTH_CONTROL;
}

od_gate_result_t od_gate_open(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                              od_span_t envelope, uint8_t *scratch, size_t scratch_cap)
{
    od_gate_result_t out;
    struct od_session_report report;
    uint16_t body_len = 0u;
    enum od_session_open rc;

    out.outcome = OD_FRAME_CRYPTO_FAILED;
    out.body = od_span_none();

    if (scratch == NULL || scratch_cap < OD_SESSION_PLAIN_MAX) {
        od_log_error("Session gate scratch is unavailable for command 0x%04X", (unsigned)cmd);
        queue_status(r, rp, cmd, RESP_NACK);
        return out;
    }

    rc = od_session_open(od_session_app_state(), cmd, envelope,
                         scratch, scratch_cap, &body_len, od_session_app_now_ms(), &report);
    /* REPORTED BEFORE THE DISPOSITION, and on the silent arm that ordering is the point: a replay
     * draws no frame, so telemetry placed after the decision to stay quiet disappears on exactly
     * the path that produces it routinely. */
    od_session_app_report(OD_SESSION_APP_OPEN, (int)rc, cmd, &report);

    switch (rc) {
    case OD_SESSION_OPEN_OK:
        out.outcome = OD_FRAME_ACCEPTED;
        out.body = od_span_make(scratch, body_len);
        return out;

    case OD_SESSION_OPEN_NO_SESSION:
        /* Not a crypto failure: the peer has not authenticated, which is the one refusal that
         * advances the link's auth-abuse run. */
        queue_status(r, rp, cmd, RESP_AUTH_REQUIRED);
        out.outcome = OD_FRAME_AUTH_REQUIRED;
        return out;

    case OD_SESSION_OPEN_REPLAY:
        /* A replayed or out-of-window PIPE DATA frame is ORDINARY PACKET LOSS, and the answer is
         * silence. pipe-write-protocol.md 5.1 makes a 0x81 NACK unconditionally fatal, so answering
         * kills the upload on the first dropped frame: the client raises IntegrityCheckError, which
         * its send loop does not catch. Saying nothing leaves the seq absent from the next SACK and
         * the host retransmits under a fresh higher counter, which the window accepts.
         *
         * Deliberately narrow -- only this opcode, and only for a NONCE reason. A tag failure is
         * tamper evidence and keeps its NACK; 0x0071's ACK discipline differs and is left alone. */
        if (cmd == CMD_PIPE_WRITE_DATA &&
            (report.nonce_reason == (uint8_t)NONCE_REPLAY ||
             report.nonce_reason == (uint8_t)NONCE_OUT_OF_WINDOW)) {
            out.outcome = OD_FRAME_CRYPTO_DROPPED;
            return out;
        }
        queue_status(r, rp, cmd, RESP_NACK);
        return out;

    case OD_SESSION_OPEN_WRONG_SESSION:
    case OD_SESSION_OPEN_BAD_TAG:
    case OD_SESSION_OPEN_BAD_LENGTH:
    case OD_SESSION_OPEN_SHORT:
    case OD_SESSION_OPEN_TOO_LONG:
    case OD_SESSION_OPEN_CRYPTO_ERROR:
        /* od_session has already applied its own strike policy -- only BAD_TAG counts, and nonce
         * and engine failures count nothing. Dispatch adds no second strike and does not advance
         * the link-level abuse run for any of these. */
        queue_status(r, rp, cmd, RESP_NACK);
        return out;

    case OD_SESSION_OPEN_NO_ROOM:
        /* Invariant failure: the caller's scratch was checked above. Answered like any other
         * crypto refusal, and the scratch is NOT read -- on a short buffer it holds nothing
         * meaningful. */
        queue_status(r, rp, cmd, RESP_NACK);
        return out;
    }
    queue_status(r, rp, cmd, RESP_NACK);
    return out;
}
