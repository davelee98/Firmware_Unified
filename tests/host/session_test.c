/* session_test.c -- shared/core/od_session.c against the fake crypto HAL in session_fake.c.
 *
 * WITH THE C0 CAPTURE SKIPPED, THE REFERENCE IS A TRANSCRIPTION. The differential layers
 * below compare od_session.c against code transcribed from the shipped targets, not against bytes
 * a device emitted. A misreading of the shipped code is therefore invisible: both sides are wrong
 * the same way. Hardware at C5/C6 is the only check that the wire did not move.
 */

#include "od_session.h"

#include "session_fake.h"
#include "aes128.h"   /* the KDF differential drives the AES core directly */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------------- harness --- */

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)

#define CASE(name) (g_case = (name))



static void test_handshake_challenge_then_proof(void)
{
    struct od_session s;
    uint8_t sn[16];

    CASE("step 1 then step 2 opens a session");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_authenticated(&s));
    CHECK(s.key_loaded);
    CHECK(g_key_set_calls == 1u);
}

static void test_wrong_proof_rejected(void)
{
    struct od_session s;
    uint8_t sn[16];

    CASE("wrong proof: REJECTED, no session");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, true) == OD_SESSION_AUTH_REJECTED);
    CHECK(!od_session_authenticated(&s));
    /* the challenge is spent, so the same nonce cannot be ground against */
    CHECK(!s.challenge_pending);
}

static void test_kdf_matches_transcription(void)
{
    struct od_session s;
    uint8_t sn[16];
    static const uint8_t CLIENT_NONCE[16] = {
        0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf
    };
    uint8_t in[58], mid[16], fin[16], want[16];
    size_t off = 0;
    unsigned i;

    CASE("KDF agrees with an independent transcription of the shipped chain");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);

    /* Transcribed from ../Firmware/src/encryption.cpp deriveSessionKey, not from od_session.c. */
    memcpy(in + off, "OpenDisplay session", 19u); off += 19u;
    in[off++] = 0x00u;
    memcpy(in + off, DEVICE_ID, 4u); off += 4u;
    memcpy(in + off, CLIENT_NONCE, 16u); off += 16u;
    memcpy(in + off, sn, 16u); off += 16u;
    in[off++] = 0x00u;
    in[off++] = 0x80u;
    CHECK(off == 58u);
    host_cmac(MASTER, in, (uint32_t)off, mid);
    memset(fin, 0, 8u);
    fin[7] = 0x01u;                       /* BE64(1) */
    memcpy(fin + 8, mid, 8u);
    od_test_aes128_encrypt(MASTER, fin, want);

    /* The fake HAL holds whatever od_session installed, so the derived key is observable. */
    CHECK(memcmp(g_slot_key[0], want, 16u) == 0);
    for (i = 0; i < 16u; ++i) { if (g_slot_key[0][i] != want[i]) { break; } }
}

static void test_challenge_expiry_boundary(void)
{
    struct od_session s;
    uint8_t rsp[OD_SESSION_REPLY_MAX];
    uint16_t rl = 0;
    uint8_t b1[1] = { 0x00 };
    uint8_t b2[32];

    CASE("challenge at exactly 30000 accepted, 30001 expired");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 0u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_CHALLENGE);
    memset(b2, 0, sizeof b2);
    /* exactly 30000: not expired, so a bad proof reports REJECTED rather than EXPIRED */
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b2, 32), 30000u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_REJECTED);

    fake_reset(); od_session_init(&s, 0);
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 0u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_CHALLENGE);
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b2, 32), 30001u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_EXPIRED);
}

static void test_rate_limit(void)
{
    struct od_session s;
    uint8_t rsp[OD_SESSION_REPLY_MAX];
    uint16_t rl = 0;
    uint8_t b1[1] = { 0x00 };
    unsigned i;

    CASE("10 attempts inside the window, then RATE_LIMITED");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    for (i = 0; i < 10u; ++i) {
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u + i,
                                      rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_CHALLENGE);
    }
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1010u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_RATE_LIMITED);

    CASE("the window resets on a 60 s IDLE GAP, measured from the LAST attempt");
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1010u + 60000u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_CHALLENGE);
}

static void test_rate_limited_keeps_challenge(void)
{
    struct od_session s;
    uint8_t rsp[OD_SESSION_REPLY_MAX];
    uint16_t rl = 0;
    uint8_t b1[1] = { 0x00 };
    unsigned i;

    CASE("RATE_LIMITED does not consume the pending challenge");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    for (i = 0; i < 10u; ++i) {
        (void)od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                      rsp, sizeof rsp, &rl, NULL);
    }
    CHECK(s.challenge_pending);
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_RATE_LIMITED);
    CHECK(s.challenge_pending);   /* still answerable once the window clears */
}

static void test_step1_over_live_session(void)
{
    struct od_session s;
    uint8_t sn[16];

    CASE("step 1 over a live session clears it and re-challenges");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(handshake(&s, 2000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_authenticated(&s));
}

static void test_capacity_is_transactional(void)
{
    struct od_session s;
    uint8_t rsp[32];
    uint16_t rl;
    uint8_t b1[1] = { 0x00 };
    size_t cap;

    CASE("rsp_cap 0..22: NO_ROOM or BAD_ARGUMENT, and no challenge is minted");
    for (cap = 0; cap < OD_SESSION_STEP1_REPLY_LEN; ++cap) {
        enum od_session_auth r;
        fake_reset(); sec_init(0);
        od_session_init(&s, 0);
        rl = 0xFFFFu;
        r = od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                    rsp, cap, &rl, NULL);
        CHECK(r == OD_SESSION_AUTH_NO_ROOM || r == OD_SESSION_AUTH_NOT_CONFIGURED);
        CHECK(!s.challenge_pending);          /* nothing minted */
        CHECK(g_key_set_calls == 0u);         /* nothing keyed */
    }
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                  rsp, OD_SESSION_STEP1_REPLY_LEN, &rl, NULL)
          == OD_SESSION_AUTH_CHALLENGE);
}

/* ------------------------------------------------- regressions for the landed-code review --- */

/* Full-state identity, so a "transactional" claim is tested as written rather than by sampling
 * the two or three fields the author happened to think of. */
static bool session_state_equal(const struct od_session *a, const struct od_session *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* OD-S2. The rate limiter used to run BEFORE the step-specific capacity preflight, so a call that
 * could not possibly reply still spent an attempt and opened the 60 s idle window. Enough of them
 * and a peer is rate-limited having never been answered once. */
static void test_no_room_mutates_nothing(void)
{
    struct od_session s, before;
    uint8_t rsp[32];
    uint16_t rl;
    uint8_t b1[1] = { 0x00 };
    uint8_t b2[OD_SESSION_STEP2_BODY_LEN];
    uint8_t server_nonce[16];
    size_t cap;
    unsigned k;

    CASE("step 1: every rsp_cap below 23 leaves the session bit-identical");
    for (cap = 0; cap < OD_SESSION_STEP1_REPLY_LEN; ++cap) {
        fake_reset(); sec_init(0);
        od_session_init(&s, 0);
        before = s;
        rl = 0xFFFFu;
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                      rsp, cap, &rl, NULL) == OD_SESSION_AUTH_NO_ROOM);
        CHECK(session_state_equal(&s, &before));
        CHECK(rl == 0u);
        CHECK(g_key_set_calls == 0u);
    }

    CASE("step 2: every rsp_cap below 19 leaves the session bit-identical");
    for (cap = 0; cap < OD_SESSION_STEP2_REPLY_LEN; ++cap) {
        fake_reset(); sec_init(0);
        od_session_init(&s, 0);
        /* A real challenge first, so the short-capacity call is a genuine step 2 rather than a
         * request refused earlier for having no challenge behind it. */
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                      rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_CHALLENGE);
        memcpy(server_nonce, rsp + 3, 16u);
        memset(b2, 0xA5, sizeof b2);
        before = s;
        rl = 0xFFFFu;
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID,
                                      od_span_make(b2, sizeof b2), 1000u,
                                      rsp, cap, &rl, NULL) == OD_SESSION_AUTH_NO_ROOM);
        CHECK(session_state_equal(&s, &before));   /* challenge still pending, attempts unmoved */
        CHECK(s.challenge_pending);
        CHECK(g_key_set_calls == 0u);          /* refused before any key was derived */
    }

    CASE("repeated NO_ROOM never reaches RATE_LIMITED");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    for (k = 0; k < OD_SESSION_RATE_MAX_ATTEMPTS * 3u; ++k) {
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                      rsp, 3u, &rl, NULL) == OD_SESSION_AUTH_NO_ROOM);
    }
    /* A full-capacity call still works, which it would not if the window had opened. */
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_CHALLENGE);
}

/* OD-S3. One challenge answers one step 2. Four failure paths used to leave it pending, so a peer
 * could re-present against the same server nonce after malformed input or an engine fault. */
static void test_challenge_is_consumed_on_every_failure(void)
{
    struct od_session s;
    uint8_t rsp[32];
    uint16_t rl;
    uint8_t b1[1] = { 0x00 };
    uint8_t b2[OD_SESSION_STEP2_BODY_LEN];
    uint8_t junk[7];
    uint8_t server_nonce[16];

    memset(b2, 0x5A, sizeof b2);
    memset(junk, 0x11, sizeof junk);

    CASE("a malformed body spends a pending challenge");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));
    CHECK(s.challenge_pending);
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(junk, sizeof junk), 1000u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_MALFORMED);
    CHECK(!s.challenge_pending);
    CHECK(s.challenge_ms == 0u);

    CASE("a wrong proof spends it");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b2, sizeof b2), 1000u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_REJECTED);
    CHECK(!s.challenge_pending);

    CASE("an engine fault mid-step-2 spends it, and reports the real status");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));
    g_force_status = OD_HAL_CRYPTO_UNSUPPORTED;      /* the very next HAL call fails */
    {
        struct od_session_report rep;
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b2, sizeof b2), 1000u,
                                      rsp, sizeof rsp, &rl, &rep)
              == OD_SESSION_AUTH_CRYPTO_ERROR);
        /* OD-S6: this used to report OD_HAL_CRYPTO_OK -- enum zero, what report_reset leaves --
         * on every engine failure, hiding exactly the evidence first hardware needs. */
        CHECK(rep.crypto_status == OD_HAL_CRYPTO_UNSUPPORTED);
    }
    CHECK(!s.challenge_pending);
    g_force_status = OD_HAL_CRYPTO_OK;

    CASE("expiry spends it; rate limiting does NOT");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b2, sizeof b2),
                                  1000u + OD_SESSION_CHALLENGE_WINDOW_MS + 1u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_EXPIRED);
    CHECK(!s.challenge_pending);

    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));
    {
        unsigned k;
        for (k = 0; k < OD_SESSION_RATE_MAX_ATTEMPTS + 2u; ++k) {
            (void)od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 1000u,
                                          rsp, sizeof rsp, &rl, NULL);
        }
    }
    /* Throttled, but a client that already holds a challenge must still be able to answer it once
     * the window clears -- so RATE_LIMITED must never have consumed one. */
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b2, sizeof b2), 1000u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_RATE_LIMITED);
    CHECK(s.challenge_pending);
}

/* OD-S5. plain_frame.n is a size_t and used to be narrowed to uint16_t BEFORE the bounds test, so
 * a length whose low 16 bits happened to be small passed and sealed the wrong thing. */
static void test_seal_rejects_wrapping_lengths(void)
{
    struct od_session s;
    uint8_t server_nonce[16];
    uint8_t out[OD_SESSION_SEALED_MAX];
    uint8_t frame[OD_SESSION_PLAIN_FRAME_MAX];
    uint16_t out_len;
    uint64_t tx_before;
    static const size_t WRAPPERS[] = { 65538u, 65536u + 2u, 0x10000u, (size_t)-1 };
    unsigned k;

    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, server_nonce, false) == OD_SESSION_AUTH_ESTABLISHED);
    memset(frame, 0x42, sizeof frame);

    CASE("exactly OD_SESSION_PLAIN_FRAME_MAX seals; one more is TOO_LONG");
    CHECK(od_session_seal(&s, od_span_make(frame, OD_SESSION_PLAIN_FRAME_MAX),
                          out, sizeof out, &out_len, 2000u, NULL) == OD_SESSION_SEAL_OK);
    CHECK(out_len == OD_SESSION_SEALED_MAX);
    tx_before = s.tx_counter;
    CHECK(od_session_seal(&s, od_span_make(frame, OD_SESSION_PLAIN_FRAME_MAX + 1u),
                          out, sizeof out, &out_len, 2000u, NULL) == OD_SESSION_SEAL_TOO_LONG);
    CHECK(s.tx_counter == tx_before);        /* a length error must not burn a counter */

    CASE("lengths that wrap a uint16_t are TOO_LONG, not silently truncated");
    for (k = 0; k < sizeof WRAPPERS / sizeof WRAPPERS[0]; ++k) {
        tx_before = s.tx_counter;
        /* The span is deliberately longer than `frame`; seal must reject on LENGTH before it
         * dereferences anything, which is the property under test. */
        CHECK(od_session_seal(&s, od_span_make(frame, WRAPPERS[k]),
                              out, sizeof out, &out_len, 2000u, NULL)
              == OD_SESSION_SEAL_TOO_LONG);
        CHECK(s.tx_counter == tx_before);
    }
}

/* A large but valid out_cap used to be cast straight to uint16_t on the way to the HAL, so a
 * buffer whose capacity ARITHMETIC wrapped produced a spurious crypto error -- and on the seal
 * side burned a tx counter doing it. The two constants below are chosen so the wrap actually
 * happens: open casts out_cap directly, so 65536 -> 0; seal casts out_cap - 18, so 65554 -> 0.
 * Sizes that merely look huge (65540, 70000) do NOT wrap and prove nothing -- the first draft of
 * this case used them and passed against the unfixed code. */
#define OD_WRAP_CAP_OPEN 65536u
#define OD_WRAP_CAP_SEAL 65554u

static void test_huge_out_cap_is_not_narrowed(void)
{
    struct od_session s;
    uint8_t server_nonce[16];
    /* Real storage behind each advertised cap. Sharing one buffer and offsetting into it would
     * advertise a capacity the tail does not have -- safe only because seal writes 7 bytes, which
     * is exactly the kind of luck a test should not depend on. */
    static uint8_t sealed[OD_WRAP_CAP_SEAL + 16u];
    static uint8_t opened[OD_WRAP_CAP_OPEN + 16u];
    uint8_t frame[8];
    uint16_t sealed_len, out_len;
    uint64_t tx_before;

    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, server_nonce, false) == OD_SESSION_AUTH_ESTABLISHED);
    memcpy(frame, "\x00\x71\x01\x02\x03\x04\x05\x06", 8u);

    CASE("seal with an out_cap whose capacity arithmetic wraps still succeeds");
    tx_before = s.tx_counter;
    CHECK(od_session_seal(&s, od_span_make(frame, sizeof frame),
                          sealed, OD_WRAP_CAP_SEAL, &sealed_len, 2000u, NULL)
          == OD_SESSION_SEAL_OK);
    CHECK(sealed_len == sizeof frame + 29u);
    CHECK(s.tx_counter == tx_before + 1u);   /* one counter spent, not one wasted on a fake error */

    CASE("open with an out_cap that narrows to zero still decrypts");
    CHECK(od_session_open(&s, 0x0071u, od_span_make(sealed + 2, (size_t)(sealed_len - 2u)),
                          opened, OD_WRAP_CAP_OPEN, &out_len, 2000u, NULL)
          == OD_SESSION_OPEN_OK);
    CHECK(out_len == sizeof frame - 2u);
    CHECK(memcmp(opened, frame + 2, out_len) == 0);
}

static void test_clock_starting_at_zero(void)
{
    struct od_session s;
    uint8_t sn[16];

    CASE("now_ms == 0 is a real time, not 'no challenge'");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 0u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
}

/* ------------------------------------------------------------------------ the replay window --- */

/* Brute-force oracle: a plain list of accepted counters, consulted linearly. Deliberately the
 * dumbest possible model -- the point is that it shares no code with the bitmap. */
#define MODEL_MAX 4096
static uint64_t g_model[MODEL_MAX];
static unsigned g_model_n;
static uint64_t g_model_last;

static void model_reset(void) { g_model_n = 0; g_model_last = 0; }
static bool model_seen(uint64_t c)
{
    unsigned i;
    for (i = 0; i < g_model_n; ++i) { if (g_model[i] == c) { return true; } }
    return false;
}
static bool model_check(uint64_t c)
{
    if (c > g_model_last) { return true; }                    /* forward: unbounded */
    if ((g_model_last - c) >= OD_NONCE_BACKWARD_BITS) { return false; }  /* too far behind */
    return !model_seen(c);
}
static void model_commit(uint64_t c)
{
    if (!model_seen(c) && g_model_n < MODEL_MAX) { g_model[g_model_n++] = c; }
    if (c > g_model_last) { g_model_last = c; }
}

static void test_replay_bitmap_sweep(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;
    uint32_t rnd = 12345u;
    unsigned i;

    CASE("bitmap agrees with a brute-force seen-set model over 10000 frames");
    memset(bm, 0, sizeof bm);
    model_reset();
    for (i = 0; i < 10000u; ++i) {
        uint64_t c;
        bool want, got;
        rnd = rnd * 1103515245u + 12345u;
        /* mostly forward, with backfills and far-behind attempts mixed in */
        if ((rnd >> 16) % 4u == 0u && last > 300u) {
            c = last - (uint64_t)((rnd >> 8) % 400u);
        } else {
            c = last + (uint64_t)((rnd >> 8) % 3u);
        }
        want = model_check(c);
        got  = (od_nonce_check(bm, last, c) == NONCE_OK);
        CHECK(want == got);
        if (got) { od_nonce_commit(bm, &last, c); model_commit(c); }
        if (last != g_model_last) { CHECK(last == g_model_last); break; }
    }
}

static void test_replay_exact_counter(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;

    CASE("a replay at exactly last_seen is refused (the diff==0 hole, closed upstream)");
    memset(bm, 0, sizeof bm);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_OK);
    od_nonce_commit(bm, &last, 0u);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_REPLAY);

    CHECK(od_nonce_check(bm, last, 5u) == NONCE_OK);
    od_nonce_commit(bm, &last, 5u);
    CHECK(last == 5u);
    CHECK(od_nonce_check(bm, last, 5u) == NONCE_REPLAY);
}

static void test_replay_counter_zero_backfill(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;

    CASE("counter 0 arriving after 1 is accepted once (a clear bit, not a sentinel)");
    memset(bm, 0, sizeof bm);
    CHECK(od_nonce_check(bm, last, 1u) == NONCE_OK);
    od_nonce_commit(bm, &last, 1u);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_OK);     /* never seen */
    od_nonce_commit(bm, &last, 0u);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_REPLAY); /* now it has been */
    CHECK(last == 1u);                                   /* a backfill never moves last_seen */
}

static void test_replay_window_boundaries(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;

    CASE("backward width: 255 behind is in, 256 behind is out; forward is unbounded");
    memset(bm, 0, sizeof bm);
    od_nonce_commit(bm, &last, 1000u);
    CHECK(od_nonce_check(bm, last, 1000u - 255u) == NONCE_OK);
    CHECK(od_nonce_check(bm, last, 1000u - 256u) == NONCE_OUT_OF_WINDOW);
    /* No forward cap at all: a huge jump is accepted, which is what stops a session stranding. */
    CHECK(od_nonce_check(bm, last, 1000u + 1000000u) == NONCE_OK);
    CHECK(od_nonce_check(bm, last, UINT64_MAX) == NONCE_OK);
}

static void test_window_not_advanced_on_bad_tag(void)
{
    struct od_session s;
    uint8_t sn[16], plain[64], sealed[300], out[300];
    uint16_t sealed_len = 0, out_len = 0;
    uint64_t before;

    CASE("a corrupt tag leaves rx_last unmoved and does not count nonce failures");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);

    plain[0] = 0x00u; plain[1] = 0x70u;
    memset(plain + 2, 0xAB, 8u);
    CHECK(od_session_seal(&s, od_span_make(plain, 10), sealed, sizeof sealed, &sealed_len,
                          1000u, NULL) == OD_SESSION_SEAL_OK);
    before = s.rx_last;
    /* feed our own sealed frame back with a corrupted tag */
    sealed[sealed_len - 1] ^= 0x01u;
    CHECK(od_session_open(&s, 0x0070u, od_span_make(sealed + 2, (size_t)(sealed_len - 2)),
                          out, sizeof out, &out_len, 1000u, NULL) == OD_SESSION_OPEN_BAD_TAG);
    CHECK(s.rx_last == before);
    CHECK(s.integrity_failures == 1u);   /* the tag IS a strike */
}

/* ---------------------------------------------------------------------------- the envelope --- */

/* Seal, then feed the result back through open. The device is talking to itself, which is legal
 * here only because inbound and outbound share one session_id and counter space -- the protocol
 * flaw recorded in od_session.h. It makes a round trip expressible in one session. */
static enum od_session_open roundtrip(struct od_session *s, const uint8_t *payload, uint16_t n,
                                      uint8_t *out, uint16_t *out_len)
{
    uint8_t plain[300], sealed[300];
    uint16_t sealed_len = 0;

    plain[0] = 0x00u; plain[1] = 0x70u;
    if (n > 0u) { memcpy(plain + 2, payload, n); }
    if (od_session_seal(s, od_span_make(plain, (size_t)(n + 2u)), sealed, sizeof sealed,
                        &sealed_len, 1000u, NULL) != OD_SESSION_SEAL_OK) {
        return OD_SESSION_OPEN_CRYPTO_ERROR;
    }
    return od_session_open(s, 0x0070u, od_span_make(sealed + 2, (size_t)(sealed_len - 2)),
                           out, 300u, out_len, 1000u, NULL);
}

static void test_envelope_roundtrip(void)
{
    struct od_session s;
    uint8_t sn[16], payload[64], out[300];
    uint16_t out_len = 0;
    unsigned i;

    CASE("seal then open returns the payload unchanged");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    for (i = 0; i < sizeof payload; ++i) { payload[i] = (uint8_t)(i * 7u); }
    CHECK(roundtrip(&s, payload, 64u, out, &out_len) == OD_SESSION_OPEN_OK);
    CHECK(out_len == 64u);
    CHECK(memcmp(out, payload, 64u) == 0);

    CASE("a zero-length payload round-trips");
    CHECK(roundtrip(&s, payload, 0u, out, &out_len) == OD_SESSION_OPEN_OK);
    CHECK(out_len == 0u);
}

static void test_envelope_min_and_max(void)
{
    struct od_session s;
    uint8_t sn[16], buf[300], out[300];
    uint16_t out_len = 0;

    CASE("envelope 28 is SHORT, 252 is TOO_LONG, both refused before the cipher");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    memset(buf, 0, sizeof buf);
    CHECK(od_session_open(&s, 0x0070u, od_span_make(buf, OD_SESSION_ENVELOPE_MIN - 1u),
                          out, sizeof out, &out_len, 1000u, NULL) == OD_SESSION_OPEN_SHORT);
    CHECK(od_session_open(&s, 0x0070u, od_span_make(buf, OD_SESSION_ENVELOPE_MAX + 1u),
                          out, sizeof out, &out_len, 1000u, NULL) == OD_SESSION_OPEN_TOO_LONG);
}

static void test_inner_length_exact(void)
{
    struct od_session s;
    uint8_t sn[16], plain[64], sealed[300], out[300];
    uint16_t sealed_len = 0, out_len = 0;

    CASE("a declared inner length that disagrees with the payload is BAD_LENGTH");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);

    /* Seal a frame whose inner length byte we then corrupt. Because the length lives INSIDE the
     * ciphertext, altering it means re-sealing with a hand-built inner block -- so instead drive
     * the fake HAL directly to produce a legal tag over a wrong length. */
    {
        uint8_t nonce[16], aad[2], inner[16], ct[64];
        uint16_t ct_len = 0;
        memcpy(nonce, s.session_id, 8u);
        memset(nonce + 8, 0, 8u);
        nonce[15] = 0x40u;                       /* a fresh, far-forward counter */
        aad[0] = 0x00u; aad[1] = 0x70u;
        inner[0] = 9u;                           /* claims 9 payload bytes... */
        memset(inner + 1, 0xCD, 4u);             /* ...but only 4 follow */
        CHECK(od_hal_crypto_ccm_encrypt(0, nonce + 3, 13u, aad, 2u, inner, 5u,
                                        ct, sizeof ct, &ct_len) == OD_HAL_CRYPTO_OK);
        memcpy(sealed, nonce, 16u);
        memcpy(sealed + 16, ct, ct_len);
        sealed_len = (uint16_t)(16u + ct_len);
        CHECK(od_session_open(&s, 0x0070u, od_span_make(sealed, sealed_len),
                              out, sizeof out, &out_len, 1000u, NULL)
              == OD_SESSION_OPEN_BAD_LENGTH);
    }
    (void)plain;
}

static void test_seal_needs_cmd_bytes(void)
{
    struct od_session s;
    uint8_t sn[16], one[1] = { 0x00 }, out[300];
    uint16_t out_len = 0;

    CASE("plain_frame shorter than 2 is TOO_SHORT");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_seal(&s, od_span_make(one, 1), out, sizeof out, &out_len, 1000u, NULL)
          == OD_SESSION_SEAL_TOO_SHORT);
}

static void test_seal_no_room_burns_nothing(void)
{
    struct od_session s;
    uint8_t sn[16], plain[16], out[300];
    uint16_t out_len = 0;
    uint64_t before;

    CASE("NO_ROOM leaves tx_counter unmoved");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    plain[0] = 0x00u; plain[1] = 0x70u;
    before = s.tx_counter;
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, 8u, &out_len, 1000u, NULL)
          == OD_SESSION_SEAL_NO_ROOM);
    CHECK(s.tx_counter == before);

    CASE("a payload above OD_SESSION_PAYLOAD_MAX is TOO_LONG, also without spending a counter");
    {
        uint8_t big[OD_SESSION_PAYLOAD_MAX + 3u];
        memset(big, 0, sizeof big);
        CHECK(od_session_seal(&s, od_span_make(big, sizeof big), out, sizeof out, &out_len,
                              1000u, NULL) == OD_SESSION_SEAL_TOO_LONG);
        CHECK(s.tx_counter == before);
    }
}

static void test_tx_counter_exhaustion(void)
{
    struct od_session s;
    uint8_t sn[16], plain[16], out[300];
    uint16_t out_len = 0;

    CASE("at UINT64_MAX sealing refuses rather than wrapping to counter 0");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    plain[0] = 0x00u; plain[1] = 0x70u;

    s.tx_counter = UINT64_MAX - 1u;
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, sizeof out, &out_len, 1000u, NULL)
          == OD_SESSION_SEAL_OK);
    CHECK(s.tx_counter == UINT64_MAX);
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, sizeof out, &out_len, 1000u, NULL)
          == OD_SESSION_SEAL_COUNTER_EXHAUSTED);
    CHECK(s.tx_counter == UINT64_MAX);   /* never wraps to 0 */
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, sizeof out, &out_len, 1000u, NULL)
          == OD_SESSION_SEAL_COUNTER_EXHAUSTED);
}

/* ------------------------------------------------------------------------------- lifecycle --- */

static void test_timeout_absolute_and_wrap_safe(void)
{
    struct od_session s;
    uint8_t sn[16];
    uint32_t start = 0xFFFFF000u;

    CASE("absolute timeout measured across the uint32 rollover");
    fake_reset(); sec_init(10);           /* 10 s */
    od_session_init(&s, 0);
    CHECK(handshake(&s, start, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    /* 5 s later, straddling the wrap: still alive */
    CHECK(od_session_alive(&s, start + 5000u, NULL));
    /* 11 s later, also past the wrap: expired on ELAPSED time, not on the wrap itself */
    CHECK(!od_session_alive(&s, start + 11000u, NULL));
    CHECK(!od_session_authenticated(&s));
}

static void test_timeout_boundary(void)
{
    struct od_session s;
    uint8_t sn[16];

    CASE("exactly timeout_ms expires (>=), one millisecond less does not");
    fake_reset(); sec_init(10);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_alive(&s, 1000u + 9999u, NULL));

    fake_reset(); od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(!od_session_alive(&s, 1000u + 10000u, NULL));

    CASE("timeout_seconds == 0 means never expire");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_alive(&s, 1000u + 4000000000u, NULL));
}

static void test_integrity_strikes(void)
{
    struct od_session s;
    uint8_t sn[16], plain[16], sealed[300], out[300];
    uint16_t sealed_len = 0, out_len = 0;
    unsigned i;

    CASE("three bad tags tear the session down");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    plain[0] = 0x00u; plain[1] = 0x70u;
    memset(plain + 2, 0x11, 4u);
    for (i = 0; i < 3u; ++i) {
        CHECK(od_session_seal(&s, od_span_make(plain, 6), sealed, sizeof sealed, &sealed_len,
                              1000u, NULL) == OD_SESSION_SEAL_OK);
        sealed[sealed_len - 1] ^= 0x01u;
        (void)od_session_open(&s, 0x0070u, od_span_make(sealed + 2, (size_t)(sealed_len - 2)),
                              out, sizeof out, &out_len, 1000u, NULL);
    }
    CHECK(!od_session_authenticated(&s));

    CASE("three replays do NOT tear it down -- nonce failures are lossy-link evidence");
    fake_reset(); od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_seal(&s, od_span_make(plain, 6), sealed, sizeof sealed, &sealed_len,
                          1000u, NULL) == OD_SESSION_SEAL_OK);
    CHECK(od_session_open(&s, 0x0070u, od_span_make(sealed + 2, (size_t)(sealed_len - 2)),
                          out, sizeof out, &out_len, 1000u, NULL) == OD_SESSION_OPEN_OK);
    for (i = 0; i < 3u; ++i) {
        CHECK(od_session_open(&s, 0x0070u, od_span_make(sealed + 2, (size_t)(sealed_len - 2)),
                              out, sizeof out, &out_len, 1000u, NULL) == OD_SESSION_OPEN_REPLAY);
    }
    CHECK(od_session_authenticated(&s));
    CHECK(s.integrity_failures == 0u);

    CASE("three engine faults do NOT tear it down either");
    fake_reset(); od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_seal(&s, od_span_make(plain, 6), sealed, sizeof sealed, &sealed_len,
                          1000u, NULL) == OD_SESSION_SEAL_OK);
    g_force_status = OD_HAL_CRYPTO_ERROR;
    for (i = 0; i < 3u; ++i) {
        CHECK(od_session_open(&s, 0x0070u, od_span_make(sealed + 2, (size_t)(sealed_len - 2)),
                              out, sizeof out, &out_len, 1000u, NULL)
              == OD_SESSION_OPEN_CRYPTO_ERROR);
    }
    g_force_status = OD_HAL_CRYPTO_OK;
    CHECK(od_session_authenticated(&s));
}

static void test_slot_preserved_across_clear(void)
{
    struct od_session s;
    uint8_t sn[16];

    CASE("od_session_clear preserves s->slot");
    fake_reset(); sec_init(1);
    od_session_init(&s, OD_HAL_CRYPTO_KEY_SLOTS - 1u);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    od_session_clear(&s);
    CHECK(s.slot == OD_HAL_CRYPTO_KEY_SLOTS - 1u);
    CHECK(!s.key_loaded);
    /* and a second handshake still lands in the configured slot */
    CHECK(handshake(&s, 2000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(s.slot == OD_HAL_CRYPTO_KEY_SLOTS - 1u);
}

static void test_no_slot_leak(void)
{
    struct od_session s;
    uint8_t sn[16];
    unsigned i;

    CASE("1000 re-authentications leak no slot: every set is matched by a clear");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    for (i = 0; i < 1000u; ++i) {
        CHECK(handshake(&s, 1000u + (i * 100u), sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    }
    od_session_clear(&s);
    /* Each re-auth clears the previous session before keying the next, so sets and clears must
     * balance. An implementation that keyed without releasing would show sets > clears. */
    CHECK(g_key_set_calls == 1000u);
    CHECK(g_key_clear_calls >= 1000u);
}

static void test_null_safety(void)
{
    struct od_session s;
    uint8_t rsp[32], out[64];
    uint16_t rl = 0, ol = 0;
    uint8_t b1[1] = { 0x00 };

    CASE("every entry point tolerates NULL");
    fake_reset(); sec_init(0);
    od_session_init(NULL, 0);
    od_session_clear(NULL);
    od_session_touch(NULL, 0u);
    CHECK(!od_session_authenticated(NULL));
    CHECK(!od_session_alive(NULL, 0u, NULL));
    CHECK(!od_session_security_enabled(NULL));
    CHECK(!od_session_derive_tls_psk(NULL, out));
    CHECK(!od_session_derive_tls_psk(&g_sec, NULL));

    od_session_init(&s, 0);
    CHECK(od_session_authenticate(NULL, &g_sec, DEVICE_ID, od_span_make(b1, 1), 0u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_BAD_ARGUMENT);
    CHECK(od_session_authenticate(&s, &g_sec, NULL, od_span_make(b1, 1), 0u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_BAD_ARGUMENT);
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(b1, 1), 0u,
                                  NULL, 32u, &rl, NULL) == OD_SESSION_AUTH_BAD_ARGUMENT);
    /* sec == NULL is NOT a programming error: it means security is not configured. */
    CHECK(od_session_authenticate(&s, NULL, DEVICE_ID, od_span_make(b1, 1), 0u,
                                  rsp, sizeof rsp, &rl, NULL) == OD_SESSION_AUTH_NOT_CONFIGURED);
    CHECK(od_session_open(NULL, 0u, od_span_none(), out, sizeof out, &ol, 0u, NULL)
          == OD_SESSION_OPEN_SHORT);
    CHECK(od_session_seal(NULL, od_span_none(), out, sizeof out, &ol, 0u, NULL)
          == OD_SESSION_SEAL_NO_ROOM);
}

static void test_tls_psk(void)
{
    uint8_t psk[16], want[16];
    static const char label[] = "opendisplay-tls-psk";

    CASE("TLS-PSK is CMAC over a fixed label under the master key");
    sec_init(0);
    CHECK(od_session_derive_tls_psk(&g_sec, psk));
    host_cmac(MASTER, (const uint8_t *)label, (uint32_t)(sizeof label - 1u), want);
    CHECK(memcmp(psk, want, 16u) == 0);
}

int main(void)
{
    fake_reset();

    test_handshake_challenge_then_proof();
    test_wrong_proof_rejected();
    test_kdf_matches_transcription();
    test_challenge_expiry_boundary();
    test_rate_limit();
    test_rate_limited_keeps_challenge();
    test_step1_over_live_session();
    test_capacity_is_transactional();
    test_clock_starting_at_zero();
    test_no_room_mutates_nothing();
    test_challenge_is_consumed_on_every_failure();
    test_seal_rejects_wrapping_lengths();
    test_huge_out_cap_is_not_narrowed();

    test_replay_bitmap_sweep();
    test_replay_exact_counter();
    test_replay_counter_zero_backfill();
    test_replay_window_boundaries();
    test_window_not_advanced_on_bad_tag();

    test_envelope_roundtrip();
    test_envelope_min_and_max();
    test_inner_length_exact();
    test_seal_needs_cmd_bytes();
    test_seal_no_room_burns_nothing();
    test_tx_counter_exhaustion();

    test_timeout_absolute_and_wrap_safe();
    test_timeout_boundary();
    test_integrity_strikes();
    test_slot_preserved_across_clear();
    test_no_slot_leak();
    test_null_safety();
    test_tls_psk();

    printf("session: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
