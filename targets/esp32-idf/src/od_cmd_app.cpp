/* od_cmd_app.cpp -- this target's implementation of the shared dispatcher's handler seam.
 *
 * ONE SWITCH, moved rather than rewritten. The opcode-to-handler mapping and its comments are the
 * shipped ones; what changes is that the dispatcher now asks for a VERDICT instead of the switch
 * inferring acceptance from a flag set behind it, and that the reply context arrives explicitly
 * instead of through a current-origin global.
 *
 * It is one seam rather than one function per opcode, deliberately. The dispatch plan names
 * od_cmd_led_activate() and friends as scaffolding with a shrink schedule; splitting them now
 * would mean rewriting every handler signature at the same moment the ordering changes underneath
 * them. The split belongs to that plan's C11, where the transfer subsystems move behind their own
 * headers anyway.
 */

#include "od_dispatch.h"

#include "buzzer_control.h"
#include "communication.h"
#include "device_control.h"
#include "display_service.h"

#include "od_cmd_reply.h"
#include "od_hal_time.h"
#include "od_log.h"

/* C++ linkage, matching their definitions -- these are target functions, not a C ABI. */
void reboot();
void enterDFUMode();
od_cmd_result_t handleClearConfig(const od_cmd_ctx_t *ctx);

extern "C" bool od_cmd_mutates_config(uint16_t cmd)
{
    /* The set a live CONFIG_READ must exclude. It is target-side because the opcode set differs
     * per target -- Nordic has NFC, Silabs has no PIPE -- and because "mutates stored config" is a
     * statement about this firmware's handlers, not about the wire.
     *
     * CONFIG_READ itself is NOT here: the dispatcher tests for it separately, since a second read
     * is excluded for a different reason (it would restart a producer that has already promised
     * the host a chunk count). */
    switch (cmd) {
    case CMD_CONFIG_WRITE:   /* 0x0041 -- saveConfig + reloadConfigAfterSave */
    case CMD_CONFIG_CHUNK:   /* 0x0042 -- same, on completion */
    case CMD_CONFIG_CLEAR:   /* 0x0045 -- clearStoredConfig */
        return true;
    default:
        return false;
    }
}

extern "C" od_cmd_result_t od_cmd_dispatch(const od_cmd_ctx_t *ctx, uint16_t cmd, od_span_t body)
{
    /* The handlers still take (uint8_t*, uint16_t); casting away const here rather than changing
     * ~15 more signatures keeps this step to the dispatch seam. They do not write through it --
     * checked -- and the const returns with the per-opcode split at C11. */
    uint8_t *data = (uint8_t *)(uintptr_t)body.p;
    uint16_t len = (uint16_t)body.n;

    /* ------------------------------------------------- NO PIPE ON LAN (review F5) ---
     *
     * SECTION 9 rule 2 of the canonical header: the sliding-window image PIPE (0x0080 / 0x0081 /
     * 0x0082) MUST NOT be used on the LAN transport, because TCP already provides the ordered,
     * reliable, flow-controlled delivery PIPE reimplements; a host is directed to DIRECT_WRITE
     * instead. DIVERGENCE_MATRIX.md 9.4 states the same rule as a dispatcher obligation.
     *
     * HERE, NOT IN od_dispatch, and not earlier in this function by accident: this is the
     * post-gate position the rule has always occupied. Moving it ahead of the session gate would
     * answer an unauthenticated plaintext-LAN client BAD_HEADER where it is answered
     * AUTH_REQUIRED today -- a wire change, and one that leaks which opcodes exist to a peer that
     * has not authenticated.
     *
     * REFUSAL IS INERT. No transfer state is touched, no session aborted, no panel session torn
     * down: a stray PIPE frame from a confused LAN client must not disturb a BLE transfer that
     * legitimately owns the slot. Same rule LAN client refusal follows, and for the same reason. */
    if (ctx->rp.origin != OD_ORIGIN_BLE &&
        (cmd == CMD_PIPE_WRITE_START || cmd == CMD_PIPE_WRITE_DATA || cmd == CMD_PIPE_WRITE_END)) {
        od_log_error("ERROR: PIPE 0x%04X is BLE-only -- rejected (use DIRECT_WRITE)",
                     (unsigned)cmd);
        /* ERROR CODE, AND THE COMPROMISE IN IT. The 0x80 NACK shape is
         * [0xFF][0x80][OD_ERR_PIPE_START_*][0x00], and that namespace has no "wrong transport"
         * member -- 0x04 is explicitly marked unused in the canonical header. Claiming 0x04 would
         * be inventing a wire meaning unilaterally, which is exactly the divergence this repo
         * exists to prevent, and the header is frozen. BAD_HEADER is reused: it is the one
         * existing code meaning "this frame is not acceptable as sent", it invents nothing, and
         * the log line above carries the real reason. A dedicated
         * OD_ERR_PIPE_START_WRONG_TRANSPORT should be added upstream when the freeze lifts.
         *
         * 0x81 and 0x82 have no canonical error namespace at all, so they get the bare NACK shape
         * their acks already use. */
        const uint8_t err = (cmd == CMD_PIPE_WRITE_START)
                                ? (uint8_t)OD_ERR_PIPE_START_BAD_HEADER : (uint8_t)0x00;
        uint8_t nack[4] = {RESP_NACK, (uint8_t)(cmd & 0xFF), err, 0x00};
        (void)od_cmd_reply(ctx, nack, sizeof(nack));
        return OD_CMD_NACK;
    }

    switch (cmd) {
    case CMD_REBOOT:              /* 0x000F */
        /* Replies by not returning. The delay is the shipped one: it exists so a queued response
         * to an EARLIER command has a chance to leave before the reset. */
        od_hal_delay_ms(100);
        reboot();
        return OD_CMD_OK;

    case CMD_CONFIG_READ:         return handleReadConfig(ctx);
    case CMD_CONFIG_WRITE:        return handleWriteConfig(ctx, data, len);
    case CMD_CONFIG_CHUNK:        return handleWriteConfigChunk(ctx, data, len);
    case CMD_READ_MSD:            return handleReadMSD(ctx);
    case CMD_CONFIG_CLEAR:        return handleClearConfig(ctx);

    case CMD_ENTER_DFU:           /* 0x0051 -- also does not return on success */
        enterDFUMode();
        return OD_CMD_OK;

    case CMD_POWER_OFF:           return handlePowerOffCommand(ctx, data, len);
    case CMD_DEEP_SLEEP:          return handleDeepSleepCommand(ctx, data, len);

    case CMD_DIRECT_WRITE_START:  return handleDirectWriteStart(ctx, data, len);
    case CMD_DIRECT_WRITE_DATA:   return handleDirectWriteData(ctx, data, len);
    case CMD_DIRECT_WRITE_END:    return handleDirectWriteEnd(ctx, data, len);
    case CMD_LED_ACTIVATE:        return handleLedActivate(ctx, data, len);
    case CMD_LED_STOP:            return handleLedStop(ctx, data, len);
    case CMD_PARTIAL_WRITE_START: return handlePartialWriteStart(ctx, data, len);
    case CMD_BUZZER:              return handleBuzzerActivate(ctx, data, len);

    case CMD_PIPE_WRITE_START:    return handlePipeWriteStart(ctx, data, len);
    case CMD_PIPE_WRITE_DATA:     return handlePipeWriteData(ctx, data, len);
    case CMD_PIPE_WRITE_END:      return handlePipeWriteEnd(ctx, data, len);

    default:
        /* NOT a NACK. An unrecognised opcode must not stamp activity, or unknown-command traffic
         * keeps the exclusive link alive; od_frame_policy gives UNKNOWN_OPCODE no stamp and no
         * abuse movement. CMD_NFC_ENDPOINT (0x0083) lands here deliberately -- this firmware does
         * not implement it on any target. */
        return OD_CMD_UNKNOWN;
    }
}
