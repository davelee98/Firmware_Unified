/* corpus_profile_portable.c -- the portable proof profile: one definition of every od_cmd_app_*
 * hook, built from semantic knobs.
 *
 * WHAT IT PROVES, AND WHAT IT DOES NOT. Running the whole corpus through this profile proves that
 * shared dispatch validates, gates, routes and plumbs every vector correctly, and that states no
 * shipping target can be put into -- a device with the buzzer compiled out, firmware older than the
 * patch byte -- are still reachable. It does NOT prove any target's reply bytes: for a vector
 * classified `target-production` the answer here comes from this file, not from firmware. The
 * runner totals the two separately so that distinction survives into the report.
 *
 * NO EXPECTATION EVER REACHES THIS FILE. It cannot include the generated table (CMake gives that
 * include directory to the runner only) and corpus_runner.h exposes knobs, not answers. Every reply
 * below is assembled from a protocol constant and a knob, the way the firmware assembles it. If a
 * vector ever needs a byte string copied in here to pass, the vector is unsupported by this profile
 * and must fail -- that failure is the signal, not an obstacle.
 */

#include "corpus_runner.h"

#include "od_cmd_app.h"
#include "od_reply.h"
#include "opendisplay_protocol.h"

#include <string.h>

/* The targets each wrap od_reply() in a named od_cmd_reply.h seam so the plain/protected choice is
 * legible at the call site. This profile belongs to no target, so it calls the shared function
 * directly -- with the same discipline: `protect` for application responses, `plain` for control
 * and error frames, chosen by the caller and never inferred from the bytes. */
static od_txq_status_t reply(const od_cmd_ctx_t *ctx, const uint8_t *f, uint16_t n)
{
    return od_reply(ctx->r, &ctx->rp, f, n);
}

static od_txq_status_t reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *f, uint16_t n)
{
    return od_reply_plain(ctx->r, &ctx->rp, f, n);
}

/* Transfer state this profile owns, so DATA/END behave like a real transfer rather than always
 * succeeding. */
static bool s_xfer_active;

unsigned od_corpus_profile_caps(void)
{
    /* A deliberately SPARSE device: no partial, no buzzer, no power latch. That is what makes the
     * compiled-out NACK vectors reachable at all -- they are the ones no promoted target can
     * produce, because both promoted targets have those subsystems. */
    return OD_VEC_CAP_PIPE | OD_VEC_CAP_NFC | OD_VEC_CAP_CONFIG_4K | OD_VEC_CAP_RXQ;
}

/* Every hook here is a behaviour fixture, so the historical shapes are exactly
 * what it exists to reach. */
bool od_corpus_profile_is_production(void) { return false; }

const char *od_corpus_profile_name(void) { return "portable"; }

void od_corpus_profile_reset(const od_vec_t *vec)
{
    s_xfer_active = (vec->xfer_active != 0u);
}

/* The 4-byte ack shape (DIVERGENCE 1.4): {status, echo, 0x00, 0x00}. Config, LED and buzzer use
 * it; the transfer opcodes use the 2-byte form. Two widths, and a runner that assumed one would
 * break on the other -- which is why the corpus pins both. */
static od_cmd_result_t ack4(const od_cmd_ctx_t *ctx, uint8_t echo)
{
    uint8_t r[4] = { RESP_ACK, 0x00u, 0x00u, 0x00u };
    r[1] = echo;
    (void)reply(ctx, r, sizeof r);
    return OD_CMD_OK;
}

static od_cmd_result_t ack2(const od_cmd_ctx_t *ctx, uint8_t echo)
{
    uint8_t r[2] = { RESP_ACK, 0x00u };
    r[1] = echo;
    (void)reply(ctx, r, sizeof r);
    return OD_CMD_OK;
}

/* The compiled-out answer a target without a subsystem gives: {0xFF, echo, 0x07, 0x00}. Silabs is
 * the model (DIVERGENCE_MATRIX opcode coverage, 0x76/0x77 rows) and this is the shape the corpus
 * records for it. */
static od_cmd_result_t unsupported(const od_cmd_ctx_t *ctx, uint8_t echo)
{
    uint8_t r[4] = { RESP_NACK, 0x00u, 0x07u, 0x00u };
    r[1] = echo;
    (void)reply_plain(ctx, r, sizeof r);
    return OD_CMD_NACK;
}

/* ------------------------------------------------------------------ device and lifecycle --- */

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx; (void)body;
    return OD_CMD_OK;              /* a real device does not return; there is nothing to answer */
}

/* [00][43][major][minor][shaLen][sha...] and, on firmware new enough, a trailing [patch].
 * fw_patch_byte is the knob that makes the pre-patch fleet shape reachable: it is a real property
 * of a real deployed build, not a way of matching a vector. */
od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[2 + 3 + 40 + 1];
    const char *sha = (od_corpus_knobs.fw_sha != NULL) ? od_corpus_knobs.fw_sha : "";
    size_t sha_len = strlen(sha);
    uint16_t o = 0u;

    (void)body;
    if (sha_len > 40u) {
        sha_len = 40u;
    }
    r[o++] = RESP_ACK;
    r[o++] = RESP_FIRMWARE_VERSION;
    /* The two version bytes are the knob's, not a constant: the pre-patch fleet build reported
     * 0/0x41 and the current one 1/5, and the vector that cares says which. */
    r[o++] = od_corpus_knobs.fw_patch_byte ? 1u : 0x00u;
    r[o++] = od_corpus_knobs.fw_patch_byte ? 5u : 0x41u;
    r[o++] = (uint8_t)sha_len;
    memcpy(&r[o], sha, sha_len);
    o = (uint16_t)(o + sha_len);
    /* THE PATCH BYTE TRAILS, so an old host that stops after the SHA still decodes. Firmware old
     * enough omits it entirely, and that is a real deployed build rather than a way of matching a
     * vector -- which is why it is a knob. */
    if (od_corpus_knobs.fw_patch_byte) {
        r[o++] = 3u;
    }
    (void)reply_plain(ctx, r, o);
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[2 + 16];
    (void)body;
    memset(r, 0, sizeof r);
    r[0] = RESP_ACK;
    r[1] = RESP_MSD_READ;
    (void)reply(ctx, r, sizeof r);
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return ack2(ctx, RESP_ENTER_DFU);
}

od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[4] = { RESP_NACK, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED, 0x00u };
    (void)body;
    if (od_corpus_knobs.caps & OD_VEC_CAP_POWER_LATCH) {
        return ack4(ctx, RESP_POWER_OFF);          /* a latch target cuts the rail and acks */
    }
    (void)reply_plain(ctx, r, sizeof r);
    return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx; (void)body;
    return OD_CMD_OK;              /* recognised, deliberately silent */
}

/* --------------------------------------------------------------------------------- config --- */

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    if (!od_corpus_knobs.storage_ok) {
        uint8_t e[4] = { RESP_NACK, RESP_CONFIG_READ, 0x00u, 0x00u };
        (void)reply_plain(ctx, e, sizeof e);
        return OD_CMD_NACK;
    }
    return ack4(ctx, RESP_CONFIG_READ);
}

od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (body.n == 0u || !od_corpus_knobs.storage_ok) {
        uint8_t e[4] = { RESP_NACK, RESP_CONFIG_WRITE, 0x00u, 0x00u };
        (void)reply_plain(ctx, e, sizeof e);
        return OD_CMD_NACK;
    }
    return ack4(ctx, RESP_CONFIG_WRITE);
}

od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (body.n == 0u || !od_corpus_knobs.storage_ok) {
        uint8_t e[4] = { RESP_NACK, RESP_CONFIG_CHUNK, 0x00u, 0x00u };
        (void)reply_plain(ctx, e, sizeof e);
        return OD_CMD_NACK;
    }
    return ack4(ctx, RESP_CONFIG_CHUNK);
}

od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return ack4(ctx, RESP_CONFIG_CLEAR);
}

/* ------------------------------------------------------------------------------- transfer --- */

od_cmd_result_t od_cmd_app_direct_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    s_xfer_active = true;
    return ack2(ctx, 0x70u);
}

od_cmd_result_t od_cmd_app_direct_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t e[2] = { RESP_NACK, 0x71u };
    (void)body;
    if (!s_xfer_active) {
        (void)reply_plain(ctx, e, sizeof e);
        return OD_CMD_NACK;
    }
    return ack2(ctx, 0x71u);
}

/* The END ack, then the refresh status -- two frames from one dispatch, which is why the corpus
 * needed ordered reply lists at all. */
od_cmd_result_t od_cmd_app_direct_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t e[2] = { RESP_NACK, 0x72u };
    (void)body;
    if (!s_xfer_active) {
        (void)reply_plain(ctx, e, sizeof e);
        return OD_CMD_NACK;
    }
    s_xfer_active = false;
    (void)ack2(ctx, 0x72u);
    return ack2(ctx, 0x73u);       /* refresh completed */
}

od_cmd_result_t od_cmd_app_partial_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    if (!(od_corpus_knobs.caps & OD_VEC_CAP_PARTIAL)) {
        return unsupported(ctx, 0x76u);
    }
    return ack2(ctx, 0x76u);
}

od_cmd_result_t od_cmd_app_pipe_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return ack2(ctx, 0x80u);
}

od_cmd_result_t od_cmd_app_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return ack2(ctx, 0x81u);
}

od_cmd_result_t od_cmd_app_pipe_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return ack2(ctx, 0x82u);
}

/* ---------------------------------------------------------------------------- peripherals --- */

od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (body.n < 1u) {
        uint8_t e[4] = { RESP_NACK, RESP_LED_ACTIVATE_ACK, 0x01u, 0x00u };
        (void)reply_plain(ctx, e, sizeof e);
        return OD_CMD_NACK;
    }
    return ack4(ctx, RESP_LED_ACTIVATE_ACK);
}

od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return ack4(ctx, RESP_LED_STOP_ACK);
}

od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    if (!(od_corpus_knobs.caps & OD_VEC_CAP_BUZZER)) {
        return unsupported(ctx, RESP_BUZZER_ACK);
    }
    return ack4(ctx, RESP_BUZZER_ACK);
}

od_cmd_result_t od_cmd_app_nfc(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    if (!(od_corpus_knobs.caps & OD_VEC_CAP_NFC)) {
        return OD_CMD_UNKNOWN;     /* ESP32's answer: silence, never an invented error code */
    }
    return ack2(ctx, RESP_NFC_ENDPOINT);
}

/* --------------------------------------------------------- the dispatcher's own predicates --- */

bool od_cmd_mutates_config(uint16_t cmd)
{
    return cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK || cmd == CMD_CONFIG_CLEAR;
}

bool od_cmd_allow_unauthenticated(uint16_t cmd)
{
    (void)cmd;
    return false;
}
