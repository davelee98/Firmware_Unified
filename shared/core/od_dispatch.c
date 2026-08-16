/* od_dispatch.c -- see od_dispatch.h; the ordering comment there is the specification. */

#include "od_dispatch.h"

#include "od_cmd_app.h"
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

/* Dispatcher ceilings, per origin. BLE's is below the 253-byte value admission so the
 * 245..253 band is a dispatcher NACK rather than a silent transport drop -- a host can discover
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

/* KEY-LOSS RECOVERY, and the ORDER of these two tests is the whole safety argument.
 *
 * The liveness question comes FIRST, so the target is asked only about a frame that has no session
 * behind it. On a live session every command still meets the gate, so this can never be used to
 * skip decryption or to slip a plaintext command past the replay window mid-session. It is
 * reachable exactly when the device would otherwise be permanently unreachable.
 *
 * od_session_alive() rather than od_session_authenticated(): the MUTATING form, which tears down an
 * expired session as it answers. The pure predicate would leave an expired-but-uncleared session
 * looking authenticated, and the exemption would then be judged against a session already dead. */
static bool unauthenticated_exempt(uint16_t cmd)
{
    if (od_session_alive(od_session_app_state(), od_session_app_now_ms(), NULL)) {
        return false;
    }
    return od_cmd_allow_unauthenticated(cmd);
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

/* THE OPCODE MAP, and it is here rather than in each target for a reason that is not tidiness.
 * Two copies of this switch is how one target answers an opcode the other treats as unknown -- and
 * "unknown" is wire-visible: od_frame_policy() gives OD_FRAME_UNKNOWN_OPCODE no activity stamp, so
 * a recognised opcode holds an exclusive link open where an unrecognised one does not. A target
 * supplies the BEHAVIOUR of a command (od_cmd_app.h); it does not get to invent the routing.
 *
 * Every target defines every hook, so a capability a target lacks is a hook returning
 * OD_CMD_UNKNOWN rather than an absent case -- and adding an opcode here without every target
 * stating what it does about it is a LINK ERROR. That is the enforcement.
 *
 * AUTHENTICATE and FIRMWARE_VERSION are absent: both are routed by od_dispatch_frame() before this
 * point, the first into od_gate and the second pre-gate. */
static od_cmd_result_t dispatch_plain(const od_cmd_ctx_t *ctx, uint16_t cmd, od_span_t body)
{
    switch (cmd) {
    case CMD_REBOOT:              return od_cmd_app_reboot(ctx, body);
    case CMD_CONFIG_READ:         return od_cmd_app_config_read(ctx, body);
    case CMD_CONFIG_WRITE:        return od_cmd_app_config_write(ctx, body);
    case CMD_CONFIG_CHUNK:        return od_cmd_app_config_chunk(ctx, body);
    case CMD_READ_MSD:            return od_cmd_app_read_msd(ctx, body);
    case CMD_CONFIG_CLEAR:        return od_cmd_app_config_clear(ctx, body);
    case CMD_ENTER_DFU:           return od_cmd_app_enter_dfu(ctx, body);
    case CMD_POWER_OFF:           return od_cmd_app_power_off(ctx, body);
    case CMD_DEEP_SLEEP:          return od_cmd_app_deep_sleep(ctx, body);
    case CMD_DIRECT_WRITE_START:  return od_cmd_app_direct_start(ctx, body);
    case CMD_DIRECT_WRITE_DATA:   return od_cmd_app_direct_data(ctx, body);
    case CMD_DIRECT_WRITE_END:    return od_cmd_app_direct_end(ctx, body);
    case CMD_LED_ACTIVATE:        return od_cmd_app_led_activate(ctx, body);
    case CMD_LED_STOP:            return od_cmd_app_led_stop(ctx, body);
    case CMD_PARTIAL_WRITE_START: return od_cmd_app_partial_start(ctx, body);
    case CMD_BUZZER:              return od_cmd_app_buzzer(ctx, body);
    case CMD_PIPE_WRITE_START:    return od_cmd_app_pipe_start(ctx, body);
    case CMD_PIPE_WRITE_DATA:     return od_cmd_app_pipe_data(ctx, body);
    case CMD_PIPE_WRITE_END:      return od_cmd_app_pipe_end(ctx, body);
    case CMD_NFC_ENDPOINT:        return od_cmd_app_nfc(ctx, body);
    default:                      return OD_CMD_UNKNOWN;
    }
}

/* Turn a handler's verdict into the dispatcher's conclusion. One place, because the mapping is
 * policy: UNKNOWN is NOT folded into NACK -- an unrecognised opcode must not stamp activity, or
 * unknown-command traffic keeps an exclusive link alive. */
static od_frame_outcome_t outcome_of(od_cmd_result_t rc)
{
    switch (rc) {
    case OD_CMD_OK:            return OD_FRAME_ACCEPTED;
    case OD_CMD_NACK:          return OD_FRAME_HANDLER_NACK;
    case OD_CMD_AUTH_REJECTED: return OD_FRAME_AUTH_REQUIRED;
    case OD_CMD_UNKNOWN:       return OD_FRAME_UNKNOWN_OPCODE;
    }
    /* No default above, so -Wswitch fails the build on a new od_cmd_result_t. Unreachable. */
    return OD_FRAME_HANDLER_NACK;
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
         * between this and the 253-byte value admission tells a host its frame was too big. */
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
        const od_cmd_result_t rc = od_cmd_app_firmware_version(&ctx, body);
        od_txq_release(&r);
        return (rc == OD_CMD_OK) ? OD_FRAME_DISCOVERY : outcome_of(rc);
    }

    /* ORIGIN-GATED DECRYPT, SECTION 9 rule 4. A frame on the TLS-PSK LAN channel is already
     * confidential and authenticated by TLS and carries NO CCM envelope, so it must not meet this
     * gate: od_session_open() would read its plaintext as a nonce and refuse it, and every ordinary
     * command inside an authenticated TLS session would answer AUTH_REQUIRED.
     *
     * The decision belongs here and cannot be pushed into od_session, which deliberately takes no
     * origin -- see od_session.h. Applying the session to a TLS frame would also advance the replay
     * counter on traffic the window never authenticated. */
    if (rp->origin != OD_ORIGIN_LAN_TLS &&
        od_session_security_enabled(od_session_app_security()) &&
        !unauthenticated_exempt(cmd)) {
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
        const od_cmd_result_t rc = dispatch_plain(&ctx, cmd, body);
        od_txq_release(&r);
        return outcome_of(rc);
    }
}
