/* od_cmd_app.cpp -- this target's implementation of shared/core/od_cmd_app.h.
 *
 * ONE FUNCTION PER OPCODE, and no switch. The opcode map is od_dispatch.c's; what is here is what
 * THIS target does about each command. A capability it lacks still gets a definition, because a
 * missing one is a link error -- which is the point: an opcode cannot be added to the shared map
 * without every target stating its answer.
 *
 * The predicates below (mutates-config, allow-unauthenticated) are questions the dispatcher asks
 * ABOUT an opcode before any handler runs, so they stay on od_dispatch.h rather than moving here.
 */

#include "od_cmd_app.h"

#include "buzzer_control.h"
#include "communication.h"
#include "device_control.h"
#include "display_service.h"
#include "od_dispatch.h"
#include "od_xfer.h"

#include "od_cmd_reply.h"
#include "od_hal_time.h"
#include "od_log.h"

/* C++ linkage, matching their definitions -- these are target functions, not a C ABI. */
void reboot();
void enterDFUMode();
od_cmd_result_t handleClearConfig(const od_cmd_ctx_t *ctx);

/* The handlers still take (uint8_t*, uint16_t). Casting away const at the seam rather than
 * changing ~15 more signatures keeps this step to the routing change; they do not write through
 * it, which is checked. */
static inline uint8_t *bytes(od_span_t body) { return (uint8_t *)(uintptr_t)body.p; }
static inline uint16_t count(od_span_t body) { return (uint16_t)body.n; }

/* ------------------------------------------------------------------- device and lifecycle --- */

extern "C" od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx; (void)body;
    /* Replies by not returning. The delay is the shipped one: it exists so a queued response to an
     * EARLIER command has a chance to leave before the reset. */
    od_hal_delay_ms(100);
    reboot();
    return OD_CMD_OK;
}

extern "C" od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleFirmwareVersion(ctx);
}

extern "C" od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleReadMSD(ctx);
}

extern "C" od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx; (void)body;
    enterDFUMode();                   /* also does not return on success */
    return OD_CMD_OK;
}

extern "C" od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handlePowerOffCommand(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleDeepSleepCommand(ctx, bytes(body), count(body));
}

/* ------------------------------------------------------------------------------- config --- */

extern "C" od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleReadConfig(ctx);
}

extern "C" od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleWriteConfig(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleWriteConfigChunk(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleClearConfig(ctx);
}

/* ----------------------------------------------------------------------------- transfer --- */

extern "C" od_cmd_result_t od_cmd_app_direct_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return od_xfer_direct_start(ctx, body);
}

extern "C" od_cmd_result_t od_cmd_app_direct_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return od_xfer_data(ctx, body);
}

extern "C" od_cmd_result_t od_cmd_app_direct_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return od_xfer_end(ctx, body);
}

extern "C" od_cmd_result_t od_cmd_app_partial_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return od_xfer_partial_start(ctx, body);
}

/* ------------------------------------------------------------- NO PIPE ON LAN (review F5) ---
 *
 * SECTION 9 rule 2 of the canonical header: the sliding-window image PIPE (0x0080 / 0x0081 /
 * 0x0082) MUST NOT be used on the LAN transport, because TCP already provides the ordered,
 * reliable, flow-controlled delivery PIPE reimplements; a host is directed to DIRECT_WRITE
 * instead. DIVERGENCE_MATRIX.md 9.4 states the same rule as a dispatcher obligation.
 *
 * INSIDE THE HOOK, NOT IN od_dispatch, and that is the post-gate position the rule has always
 * occupied. Moving it ahead of the session gate would answer an unauthenticated plaintext-LAN
 * client BAD_HEADER where it is answered AUTH_REQUIRED today -- a wire change, and one that leaks
 * which opcodes exist to a peer that has not authenticated.
 *
 * REFUSAL IS INERT. No transfer state is touched, no session aborted, no panel session torn down:
 * a stray PIPE frame from a confused LAN client must not disturb a BLE transfer that legitimately
 * owns the slot. Same rule LAN client refusal follows, and for the same reason. */
static bool pipe_refused_on_lan(const od_cmd_ctx_t *ctx, uint16_t cmd)
{
    if (ctx->rp.origin == OD_ORIGIN_BLE) {
        return false;
    }
    od_log_error("ERROR: PIPE 0x%04X is BLE-only -- rejected (use DIRECT_WRITE)", (unsigned)cmd);
    /* ERROR CODE, AND THE COMPROMISE IN IT. The 0x80 NACK shape is
     * [0xFF][0x80][OD_ERR_PIPE_START_*][0x00], and that namespace has no "wrong transport" member
     * -- 0x04 is explicitly marked unused in the canonical header. Claiming 0x04 would be inventing
     * a wire meaning unilaterally, which is exactly the divergence this repo exists to prevent, and
     * the header is frozen. BAD_HEADER is reused: it is the one existing code meaning "this frame
     * is not acceptable as sent", it invents nothing, and the log line above carries the real
     * reason. A dedicated OD_ERR_PIPE_START_WRONG_TRANSPORT should be added upstream when the
     * freeze lifts.
     *
     * 0x81 and 0x82 have no canonical error namespace at all, so they get the bare NACK shape
     * their acks already use. */
    const uint8_t err = (cmd == CMD_PIPE_WRITE_START)
                            ? (uint8_t)OD_ERR_PIPE_START_BAD_HEADER : (uint8_t)0x00;
    uint8_t nack[4] = {RESP_NACK, (uint8_t)(cmd & 0xFF), err, 0x00};
    (void)od_cmd_reply(ctx, nack, sizeof(nack));
    return true;
}

extern "C" od_cmd_result_t od_cmd_app_pipe_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (pipe_refused_on_lan(ctx, CMD_PIPE_WRITE_START)) {
        return OD_CMD_NACK;
    }
    return handlePipeWriteStart(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (pipe_refused_on_lan(ctx, CMD_PIPE_WRITE_DATA)) {
        return OD_CMD_NACK;
    }
    return handlePipeWriteData(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_pipe_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (pipe_refused_on_lan(ctx, CMD_PIPE_WRITE_END)) {
        return OD_CMD_NACK;
    }
    return handlePipeWriteEnd(ctx, bytes(body), count(body));
}

/* -------------------------------------------------------------------------- peripherals --- */

extern "C" od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleLedActivate(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleLedStop(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleBuzzerActivate(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_nfc(const od_cmd_ctx_t *ctx, od_span_t body)
{
    /* UNKNOWN, AND SILENT. This firmware implements no NFC on this target, and manufacturing an
     * "unsupported NFC" error would be inventing a wire meaning. UNKNOWN also keeps the frame out
     * of the activity stamp, so probing 0x0083 cannot hold the exclusive link open. */
    (void)ctx; (void)body;
    return OD_CMD_UNKNOWN;
}

/* ------------------------------------------------------------------ dispatcher predicates --- */

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

extern "C" bool od_cmd_allow_unauthenticated(uint16_t cmd)
{
    /* NOTHING, on this target. Its key-loss recovery is not a gate exemption: configWriteGate()
     * runs INSIDE handleWriteConfig, so it is reachable only for frames that already passed the
     * session gate -- which in practice means the TLS-LAN channel, where the transport is the
     * authentication. Returning true for anything here would open a plaintext-BLE path that
     * ESP32 has never had. */
    (void)cmd;
    return false;
}
