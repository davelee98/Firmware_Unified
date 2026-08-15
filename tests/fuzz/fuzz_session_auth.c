/* fuzz_session_auth.c -- the 0x0050 handshake, which is THE pre-authentication surface.
 *
 * Anything that can connect over BLE can send these bytes without knowing the key, so
 * MIGRATION.md's Gate 1 names this path specifically. The body grammar is narrow -- one byte for
 * step 1, thirty-two for step 2 -- so the reachable state space is small; what this target really
 * sweeps is the interaction between a hostile body, a hostile clock and a short reply buffer.
 *
 * STEP 2 HAS TO BE ARRANGED FOR, TWICE OVER -- the proof compare, the key derivation and the
 * 19-byte capacity preflight are behind two independent gates, and missing either leaves them
 * unreachable while coverage still climbs convincingly:
 *   1. challenge_pending, which only a PRIOR step 1 on the same session sets. A harness that
 *      resets per input can otherwise only ever produce step 1. Hence the control byte.
 *   2. THE CLOCK. The challenge expires 30 s after it is minted, so a freely-chosen 32-bit
 *      now_ms is almost always outside the window and returns EXPIRED before the proof is
 *      compared. Hence base + bounded delta below.
 * Both were verified by temporarily asserting that OD_SESSION_AUTH_REJECTED -- reachable only
 * from the proof compare -- never occurs, and watching it start firing.
 *
 * The transactional property is the one worth breaking: capacity is checked BEFORE any state
 * changes, so a caller with a short buffer can never leave the device holding a session the
 * client was never told about -- an open session reachable without knowing the key.
 */

#include "fuzz_session.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct od_session s;
    struct fz_cursor cur;
    struct od_session_report report;
    uint8_t *rsp;
    uint16_t rsp_len = 0xFFFFu;
    uint32_t now_ms;
    uint32_t base;
    uint32_t delta;
    uint8_t  ctl;
    size_t   rsp_cap;
    size_t   body_n;
    const uint8_t *body;
    bool     was_auth;
    enum od_session_auth r;

    fake_reset();
    sec_init(0u);
    memset(&s, 0, sizeof s);
    od_session_init(&s, 0u);

    fz_cursor_init(&cur, data, size);

    ctl = fz_u8(&cur);
    /* THE CLOCK IS A BASE PLUS A BOUNDED DELTA, not a free 32-bit value. A free value was the
     * second reason step 2 stayed unreachable: the challenge is minted at the base and judged at
     * now_ms, so an unconstrained now_ms is almost always past the 30 s window and returns
     * EXPIRED before the proof is ever compared. A 16-bit delta straddles that window -- 29 999,
     * 30 000 and 30 001 are all a few mutations apart -- while a free base keeps the uint32_t
     * rollover and the 60 s rate window reachable. */
    base    = (uint32_t)((uint32_t)fz_u8(&cur) << 24 | (uint32_t)fz_u8(&cur) << 16 |
                         (uint32_t)fz_u8(&cur) << 8  | (uint32_t)fz_u8(&cur));
    delta   = (uint32_t)(((uint32_t)fz_u8(&cur) << 8) | fz_u8(&cur));
    now_ms  = base + delta;

    /* Bit 0: mint a challenge at the base, so the body below is judged as a STEP 2 and reaches
     * the proof compare. Clear, and the body is a step 1 against a fresh session. Both halves of
     * the machine stay reachable. */
    if ((ctl & 1u) != 0u) {
        uint8_t  ch_rsp[OD_SESSION_REPLY_MAX];
        uint16_t ch_len = 0u;
        uint8_t  step1[1] = { 0x00u };
        (void)od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(step1, 1u), base,
                                      ch_rsp, sizeof ch_rsp, &ch_len, NULL);
    }

    /* Past the 23-byte step-1 reply, so both the exact fit and the one-byte-short case are in
     * range -- every capacity from 0 to 24. */
    rsp_cap = (size_t)fz_u8(&cur) % 25u;

    body = fz_rest(&cur, &body_n);
    if (body_n > 64u) { body_n = 64u; }     /* the grammar tops out at 32; 64 covers over-long */

    was_auth = od_session_authenticated(&s);

    rsp = (uint8_t *)malloc(rsp_cap != 0u ? rsp_cap : 1u);
    FZ_ASSERT(rsp != NULL);

    r = od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(body, body_n), now_ms,
                                rsp, rsp_cap, &rsp_len, &report);

    FZ_ASSERT(r <= OD_SESSION_AUTH_BAD_ARGUMENT);
    FZ_ASSERT(rsp_len <= rsp_cap);

    /* The two reply shapes are wire contract, and they are the only discriminators between the
     * steps: AUTH_STATUS_SUCCESS and AUTH_STATUS_CHALLENGE are BOTH 0x00, so the status byte
     * cannot tell them apart. Every shipping host expects 19, not the spec's 3. */
    if (r == OD_SESSION_AUTH_CHALLENGE)   { FZ_ASSERT(rsp_len == OD_SESSION_STEP1_REPLY_LEN); }
    if (r == OD_SESSION_AUTH_ESTABLISHED) { FZ_ASSERT(rsp_len == OD_SESSION_STEP2_REPLY_LEN); }

    /* TRANSACTIONAL. A refusal for want of room must leave the session exactly as it was -- in
     * particular it must not have authenticated one the caller cannot report. */
    if (r == OD_SESSION_AUTH_NO_ROOM) {
        FZ_ASSERT(rsp_len == 0u);
        FZ_ASSERT(od_session_authenticated(&s) == was_auth);
    }

    /* No body a fuzzer can produce without the master key may open a session: the proof is a CMAC
     * under a key that is not in the input. If this ever fires, the proof compare is broken. */
    FZ_ASSERT(r != OD_SESSION_AUTH_ESTABLISHED);
    FZ_ASSERT(!od_session_authenticated(&s));

    free(rsp);
    return 0;
}
