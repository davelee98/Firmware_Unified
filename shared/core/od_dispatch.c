/* od_dispatch.c -- see od_dispatch.h; the ordering comment there is the specification. */

#include "od_dispatch.h"

#include "od_config_read.h"
#include "od_gate.h"
#include "od_reply.h"
#include "od_session.h"
#include "od_session_app.h"

#include <string.h>

/* THE DECRYPT SCRATCH, owned here. One buffer for the whole command path: the gate decrypts into
 * it and the body handed to a handler points into it, so it must outlive the handler and must not
 * be shared with anything that runs inside one. */
static uint8_t s_plain[OD_SESSION_PLAIN_MAX];

/* Dispatcher ceilings, per origin. BLE's is well below the 256-byte transport admission so the
 * 245..256 band is a dispatcher NACK rather than a silent transport drop -- a host can discover
 * the first and cannot discover the second. */
#define OD_DISPATCH_MAX_BLE  244u
#define OD_DISPATCH_MAX_LAN  4094u

/* How many response slots one opcode may need, worst case. Under-reserving is the bug that matters:
 * a handler that mutates state and then cannot answer looks to the host exactly like a command
 * that never ran.
 *
 * 0x81 is 3 because handlePipeWriteData can emit the data ACK, the END ack AND the refresh status
 * in one call. 0x71 is 2 because it emits the data ack OR calls END, never both. The END opcodes
 * are 2: their own ack, then the post-refresh status -- and that reservation is HELD ACROSS THE
 * REFRESH, which is free here because capacity is a counter rather than a parked slot. */
uint8_t od_dispatch_budget(uint16_t cmd)
{
    switch (cmd) {
    case CMD_PIPE_WRITE_DATA:      return 3u;
    case CMD_DIRECT_WRITE_DATA:    return 2u;
    case CMD_DIRECT_WRITE_END:     return 2u;
    /* THREE, not two. An explicit PIPE END emits the tail SACK (sendPipeAck), then the END ACK,
     * then the refresh status -- verified on both its partial and full branches
     * (display_service.cpp:2865, :2886, :2897/:2901 and :2327, :2373/:2377). At two the third
     * reply is dropped AFTER the panel has already refreshed, so the host loses the completion
     * signal for work the device actually did. The parent plan's table said two and was wrong. */
    case CMD_PIPE_WRITE_END:       return 3u;
    default:                       return 1u;
    }
}

static uint16_t origin_ceiling(od_origin_t origin)
{
    return (origin == OD_ORIGIN_BLE) ? OD_DISPATCH_MAX_BLE : OD_DISPATCH_MAX_LAN;
}

/* Answer [00][cmd][status] without a reservation of our own. Used only on the paths that refuse a
 * frame before any budget was claimed, where a one-slot reserve is the whole cost. */
static void refuse(const od_reply_t *rp, uint16_t cmd, uint8_t status)
{
    od_tx_reservation_t r;
    uint8_t frame[3];

    if (od_txq_reserve(1u, &r) != OD_TXQ_OK) {
        return;                       /* nothing to say it with; the host times out */
    }
    frame[0] = RESP_ACK;
    frame[1] = (uint8_t)(cmd & 0xFFu);
    frame[2] = status;
    (void)od_reply_plain(&r, rp, frame, sizeof frame);
    od_txq_release(&r);
}

od_frame_outcome_t od_dispatch_frame(const od_reply_t *rp, od_span_t frame)
{
    od_tx_reservation_t r;
    od_frame_outcome_t outcome;
    od_span_t body;
    uint16_t cmd;

    if (rp == NULL || !od_span_has(frame, 2u)) {
        return OD_FRAME_REJECTED_FRAME;   /* no opcode: nothing to echo, nothing to answer */
    }
    cmd = (uint16_t)(((uint16_t)frame.p[0] << 8) | frame.p[1]);
    body = od_span_drop(frame, 2u);

    /* ---- structural ---- */
    if (frame.n > origin_ceiling(rp->origin)) {
        /* Refused by the DISPATCHER, deliberately, rather than dropped by the transport: the band
         * between this and the 256-byte admission is where a host learns its frame was too big. */
        refuse(rp, cmd, RESP_NACK);
        return OD_FRAME_REJECTED_FRAME;
    }

    /* ---- tag liveness ---- */
    if (!od_hal_radio_tag_is_live(rp->origin, rp->tag)) {
        /* The peer that sent this is gone. Answering would deliver to whoever inherited the slot. */
        return OD_FRAME_STALE_TAG;
    }

    /* ---- producer conflict, BEFORE reserve and BEFORE decrypt ----
     * A live CONFIG_READ owns the config scratch, and a write reloads through the same buffer;
     * letting one land between two chunks splices two configs into one CRC-valid read-back.
     * Deferring here leaves the replay window and the caller's bytes untouched, which is the whole
     * reason this check cannot move below the gate. */
    if (od_config_read_active() &&
        (cmd == CMD_CONFIG_READ || od_cmd_mutates_config(cmd))) {
        return OD_FRAME_DEFERRED;
    }

    /* ---- budget and reserve ---- */
    if (od_txq_reserve(od_dispatch_budget(cmd), &r) != OD_TXQ_OK) {
        /* No capacity to answer, so the handler must not run: it would mutate state and then be
         * unable to say so. Deferred rather than refused -- the frame is still good. */
        return OD_FRAME_DEFERRED;
    }

    /* ---- gate ----
     * Everything below has a slot, including the gate's own FE/FF answers. */
    if (cmd == CMD_AUTHENTICATE) {
        outcome = od_gate_authenticate(&r, rp, body);
        od_txq_release(&r);
        return outcome;
    }

    /* FIRMWARE_VERSION IS ALWAYS PLAINTEXT-READABLE, exempt from the session gate on every target
     * (Firmware communication.cpp:488). A client must be able to learn what it is talking to
     * before it can authenticate -- and a device whose key the host has lost would otherwise be
     * unidentifiable. DISCOVERY rather than ACCEPTED, so a version poll cannot stamp activity and
     * hold the exclusive link indefinitely. */
    if (cmd == CMD_FIRMWARE_VERSION) {
        const od_cmd_ctx_t ctx = { *rp, &r };
        const od_cmd_result_t rc = od_cmd_dispatch(&ctx, cmd, body);
        od_txq_release(&r);
        return (rc == OD_CMD_OK) ? OD_FRAME_DISCOVERY : OD_FRAME_HANDLER_NACK;
    }

    if (od_session_security_enabled(od_session_app_security())) {
        const od_gate_result_t g = od_gate_open(&r, rp, cmd, body, s_plain, sizeof s_plain);
        if (g.outcome != OD_FRAME_ACCEPTED) {
            od_txq_release(&r);
            return g.outcome;
        }
        body = g.body;
    }

    /* ---- handler ---- */
    {
        const od_cmd_ctx_t ctx = { *rp, &r };
        const od_cmd_result_t rc = od_cmd_dispatch(&ctx, cmd, body);
        od_txq_release(&r);
        switch (rc) {
        case OD_CMD_OK:            return OD_FRAME_ACCEPTED;
        case OD_CMD_NACK:          return OD_FRAME_HANDLER_NACK;
        case OD_CMD_AUTH_REJECTED: return OD_FRAME_AUTH_REQUIRED;
        /* NOT folded into NACK: an unrecognised opcode must not stamp activity, or unknown-command
         * traffic keeps the exclusive link alive. od_frame_policy() gives UNKNOWN_OPCODE no
         * activity and no abuse movement. */
        case OD_CMD_UNKNOWN:       return OD_FRAME_UNKNOWN_OPCODE;
        }
    }
    /* No default above, so -Wswitch fails the build on a new od_cmd_result_t. Unreachable. */
    return OD_FRAME_HANDLER_NACK;
}
