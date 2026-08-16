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

/* THE STEP-2 REPLY BYTES. Until this case existed, nothing in the suite looked at the 16-byte
 * mutual-auth proof the device returns, nor at any status or opcode byte in any reply. That let
 * two mutations pass a full green run: keying the server proof with the MASTER key instead of the
 * session key, and swapping its nonce order. Either ships a device no shipping host can
 * authenticate, and both are wire-visible, so no host update could rescue it. */
static void test_step2_reply_bytes(void)
{
    struct od_session s;
    uint8_t server_nonce[16];
    uint8_t rsp[OD_SESSION_REPLY_MAX];
    uint16_t rl = 0;
    uint8_t proof_in[36], expect[16];

    fake_reset(); sec_init(0);
    od_session_init(&s, 0);

    CASE("step 1 reply: [ACK][0x50][CHALLENGE][server_nonce:16][device_id:4]");
    CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));
    CHECK(rl == OD_SESSION_STEP1_REPLY_LEN);
    CHECK(rsp[0] == RESP_ACK);
    CHECK(rsp[1] == RESP_AUTHENTICATE);
    CHECK(rsp[2] == AUTH_STATUS_CHALLENGE);
    CHECK(memcmp(rsp + 3, server_nonce, 16u) == 0);
    CHECK(memcmp(rsp + 19, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN) == 0);

    CASE("step 2 reply: [ACK][0x50][SUCCESS][server_proof:16], proof keyed with the SESSION key");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake_capture(&s, 1000u, server_nonce, rsp, &rl) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(rl == OD_SESSION_STEP2_REPLY_LEN);
    CHECK(rsp[0] == RESP_ACK);
    CHECK(rsp[1] == RESP_AUTHENTICATE);
    CHECK(rsp[2] == AUTH_STATUS_SUCCESS);

    /* Recomputed independently, from the slot the fake captured -- CMAC(session_key,
     * server_nonce || client_nonce || device_id). BOTH proofs are server-nonce-first; only the
     * key differs. Pinned against Firmware/src/encryption.cpp:703-705 and py-opendisplay
     * compute_server_proof(). */
    memcpy(proof_in, server_nonce, 16u);
    memcpy(proof_in + 16, CLIENT_NONCE, 16u);
    memcpy(proof_in + 32, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN);
    host_cmac(g_slot_key[0], proof_in, 36u, expect);
    CHECK(memcmp(rsp + 3, expect, 16u) == 0);

    CASE("failure replies carry the right opcode and status");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, server_nonce, true) == OD_SESSION_AUTH_REJECTED);
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    {
        uint8_t junk[7] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77 };
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(junk, sizeof junk),
                                      1000u, rsp, sizeof rsp, &rl, NULL)
              == OD_SESSION_AUTH_MALFORMED);
        CHECK(rl == 3u);
        CHECK(rsp[0] == RESP_ACK);
        CHECK(rsp[1] == RESP_AUTHENTICATE);
        CHECK(rsp[2] == AUTH_STATUS_ERROR);
    }
}

/* The timeout is ABSOLUTE from session_start_ms. Nothing used to open or seal on an EXPIRED
 * session, so re-stamping session_start_ms on every accepted frame -- turning it into an idle
 * timeout that never fires on a busy session -- was invisible to the whole suite. */
static void test_expired_session_refuses_open_and_seal(void)
{
    struct od_session s;
    uint8_t server_nonce[16];
    uint8_t sealed[OD_SESSION_SEALED_MAX];
    uint8_t plain[OD_SESSION_PLAIN_MAX];
    uint8_t frame[8] = { 0x00,0x71,1,2,3,4,5,6 };
    uint16_t sealed_len = 0, out_len = 0;
    uint32_t t = 1000u;

    fake_reset(); sec_init(10);            /* 10 s */
    od_session_init(&s, 0);
    CHECK(handshake(&s, t, server_nonce, false) == OD_SESSION_AUTH_ESTABLISHED);

    CASE("traffic does NOT extend an absolute timeout");
    /* Nine seconds of steady accepted traffic, one frame per second. */
    {
        unsigned k;
        for (k = 1; k <= 9u; ++k) {
            CHECK(od_session_seal(&s, od_span_make(frame, sizeof frame), sealed, sizeof sealed,
                                  &sealed_len, t + k * 1000u, NULL) == OD_SESSION_SEAL_OK);
            CHECK(od_session_open(&s, 0x0071u,
                                  od_span_make(sealed + 2, (size_t)(sealed_len - 2u)),
                                  plain, sizeof plain, &out_len, t + k * 1000u, NULL)
                  == OD_SESSION_OPEN_OK);
        }
    }
    /* At exactly the timeout the session is gone, however busy it has been. */
    CHECK(od_session_seal(&s, od_span_make(frame, sizeof frame), sealed, sizeof sealed,
                          &sealed_len, t + 10000u, NULL) == OD_SESSION_SEAL_NO_SESSION);
    CHECK(!od_session_authenticated(&s));

    CASE("open on an expired session is NO_SESSION, not a decrypt attempt");
    fake_reset(); sec_init(10);
    od_session_init(&s, 0);
    CHECK(handshake(&s, t, server_nonce, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_session_seal(&s, od_span_make(frame, sizeof frame), sealed, sizeof sealed,
                          &sealed_len, t, NULL) == OD_SESSION_SEAL_OK);
    CHECK(od_session_open(&s, 0x0071u, od_span_make(sealed + 2, (size_t)(sealed_len - 2u)),
                          plain, sizeof plain, &out_len, t + 10000u, NULL)
          == OD_SESSION_OPEN_NO_SESSION);
}

/* encryption_enabled is read as != 0, deliberately overriding the authority target's == 1: on a
 * security gate a corrupted byte must fail CLOSED. DIVERGENCE_MATRIX 6.9 records that as the one
 * place this promotion overrode Firmware, and nothing tested it -- sec_init only ever set 1. */
static void test_security_enabled_is_fail_safe(void)
{
    struct SecurityConfig sec;
    unsigned v;

    CASE("any non-zero encryption_enabled enables security");
    for (v = 1u; v <= 255u; ++v) {
        sec_init(0);
        sec = g_sec;
        sec.encryption_enabled = (uint8_t)v;
        CHECK(od_session_security_enabled(&sec));
    }

    CASE("zero disables it, and so does an all-zero key whatever the flag says");
    sec_init(0);
    sec = g_sec;
    sec.encryption_enabled = 0u;
    CHECK(!od_session_security_enabled(&sec));
    sec_init(0);
    sec = g_sec;
    memset(sec.encryption_key, 0, sizeof sec.encryption_key);
    CHECK(!od_session_security_enabled(&sec));
    CHECK(!od_session_security_enabled(NULL));
}

/* RFC 4493 known-answer vectors for host_cmac().
 *
 * WHY THIS IS NOT REDUNDANT WITH aes128_test.c. That suite pins the AES-128 BLOCK CIPHER against
 * FIPS-197, which is real and necessary -- but every session expectation is computed through the
 * CMAC construction layered on top, and CMAC was pinned by nothing. Breaking RFC 4493's Rb
 * constant (0x87) in session_fake.c left the entire suite green, including the KDF differential,
 * because the expected value and the actual value both flowed through the same broken primitive.
 * A differential is only a differential if the two sides share no code. */
static void test_host_cmac_rfc4493(void)
{
    static const uint8_t K[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    static const uint8_t M[64] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
        0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10
    };
    /* The four vectors of RFC 4493 section 4: len 0, 16, 40, 64. */
    static const struct { uint32_t len; uint8_t mac[16]; } V[] = {
        { 0u,  { 0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46 } },
        { 16u, { 0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44,0xf7,0x9b,0xdd,0x9d,0xd0,0x4a,0x28,0x7c } },
        { 40u, { 0xdf,0xa6,0x67,0x47,0xde,0x9a,0xe6,0x30,0x30,0xca,0x32,0x61,0x14,0x97,0xc8,0x27 } },
        { 64u, { 0x51,0xf0,0xbe,0xbf,0x7e,0x3b,0x9d,0x92,0xfc,0x49,0x74,0x17,0x79,0x36,0x3c,0xfe } }
    };
    uint8_t mac[16];
    unsigned i;

    CASE("host_cmac matches all four RFC 4493 vectors");
    for (i = 0; i < sizeof V / sizeof V[0]; ++i) {
        memset(mac, 0, sizeof mac);
        host_cmac(K, M, V[i].len, mac);
        CHECK(memcmp(mac, V[i].mac, 16u) == 0);
    }
}

/* Every step-2 crypto-failure exit, one per HAL call. Only the FIRST of these was reachable while
 * the fake's injection was all-or-nothing: a single flag is consumed by whichever call comes
 * first, so derive_session_key, derive_session_id, the all-zero-session-id rejection and the
 * server-proof/key_set exits -- including both od_session_clear() calls that stop a half-derived
 * session persisting -- had no coverage at all. Prerequisite for the dispatch plan's
 * session-result matrix, which needs one case per enum member. */
static void test_every_step2_crypto_exit(void)
{
    /* HAL call indices inside a step-2, counted from the start of the handshake:
     *   0  step-1 random (server nonce)
     *   1  derive_proof(master)          -> the expected client proof
     *   2  derive_session_key: cmac
     *   3  derive_session_key: aes_ecb
     *   4  derive_session_id:  cmac
     *   5  derive_proof(session)         -> the server proof
     *   6  key_set                       -> loads the slot
     * Each is forced in turn; all must land on CRYPTO_ERROR with no session and no live key. */
    static const struct { int32_t after; const char *what; } EXITS[] = {
        { 1, "derive_proof(master) fails" },
        { 2, "derive_session_key CMAC fails" },
        { 3, "derive_session_key AES-ECB fails" },
        { 4, "derive_session_id CMAC fails" },
        { 5, "derive_proof(session) fails" },
        { 6, "key_set fails" }
    };
    struct od_session s;
    uint8_t server_nonce[16];
    unsigned i;

    for (i = 0; i < sizeof EXITS / sizeof EXITS[0]; ++i) {
        struct od_session_report rep;
        uint8_t rsp[OD_SESSION_REPLY_MAX];
        uint16_t rl = 0;
        uint8_t body2[OD_SESSION_STEP2_BODY_LEN];
        uint8_t proof_in[36], proof[16];

        CASE(EXITS[i].what);
        fake_reset(); sec_init(0);
        od_session_init(&s, 0);
        CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));

        /* A CORRECT proof, so the run reaches the derivations rather than stopping at REJECTED. */
        memcpy(proof_in, server_nonce, 16u);
        memcpy(proof_in + 16, CLIENT_NONCE, 16u);
        memcpy(proof_in + 32, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN);
        host_cmac(MASTER, proof_in, 36u, proof);
        memcpy(body2, CLIENT_NONCE, 16u);
        memcpy(body2 + 16, proof, 16u);

        g_force_status = OD_HAL_CRYPTO_UNSUPPORTED;
        g_force_after = EXITS[i].after;
        CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(body2, sizeof body2),
                                      1000u, rsp, sizeof rsp, &rl, &rep)
              == OD_SESSION_AUTH_CRYPTO_ERROR);
        g_force_status = OD_HAL_CRYPTO_OK;
        g_force_after = 0;

        /* The reply is well-formed, the status is reported, and nothing is left half-built. */
        CHECK(rl == 3u);
        CHECK(rsp[2] == AUTH_STATUS_ERROR);
        CHECK(rep.crypto_status == OD_HAL_CRYPTO_UNSUPPORTED);
        CHECK(!od_session_authenticated(&s));
        CHECK(!s.challenge_pending);          /* the challenge is spent on every one of these */
        /* Past the server proof the code has already touched session_id/slot, so the exits there
         * must CLEAR rather than merely return -- that is what stops a half-derived session
         * looking live to od_session_alive(). */
        CHECK(!s.key_loaded);
        /* session_id must be zero on EVERY one of these exits. Past the id derivation the field
         * is populated, so the two exits after it rely on od_session_clear() to wipe it -- and
         * removing that clear is otherwise invisible, since a stale id on a session with
         * authenticated == false is inert through the public API. Pinned here so it stays
         * defence in depth rather than quietly becoming nothing. */
        {
            unsigned k, nonzero = 0;
            for (k = 0; k < OD_SESSION_ID_LEN; ++k) { nonzero |= s.session_id[k]; }
            CHECK(nonzero == 0u);
        }
    }
}

/* The all-zero session id is rejected. Reachable only by making the id derivation produce zeros,
 * which needs the fake to fail nothing and instead return a zero MAC -- so it is driven through
 * the same counter with a dedicated knob rather than an error. */
static void test_all_zero_session_id_rejected(void)
{
    struct od_session s;
    uint8_t server_nonce[16];
    uint8_t rsp[OD_SESSION_REPLY_MAX];
    uint16_t rl = 0;
    uint8_t body2[OD_SESSION_STEP2_BODY_LEN];
    uint8_t proof_in[36], proof[16];

    CASE("a derivation yielding an all-zero session id is refused, not accepted");
    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake_step1(&s, 1000u, server_nonce, rsp, &rl));
    memcpy(proof_in, server_nonce, 16u);
    memcpy(proof_in + 16, CLIENT_NONCE, 16u);
    memcpy(proof_in + 32, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN);
    host_cmac(MASTER, proof_in, 36u, proof);
    memcpy(body2, CLIENT_NONCE, 16u);
    memcpy(body2 + 16, proof, 16u);

    g_zero_cmac_after = 4;                 /* the derive_session_id CMAC returns all zeros */
    CHECK(od_session_authenticate(&s, &g_sec, DEVICE_ID, od_span_make(body2, sizeof body2),
                                  1000u, rsp, sizeof rsp, &rl, NULL)
          == OD_SESSION_AUTH_CRYPTO_ERROR);
    g_zero_cmac_after = -1;
    CHECK(!od_session_authenticated(&s));
    CHECK(!s.key_loaded);
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

/* A SEALED RESPONSE IS OUTBOUND ACTIVITY, and every way of failing to produce one is not.
 *
 * The distinction is not bookkeeping. last_activity_ms is what the targets' idle policy reads, so
 * stamping it on a refusal keeps a link alive on traffic the device declined to answer; not
 * stamping it on a success stops an active client's clock and disconnects it mid-transfer. The
 * cipher-error case is the one worth stating: it may have SPENT a counter, but it produced no
 * outbound bytes, so it is not activity. */
static void test_seal_activity_success_only(void)
{
    struct od_session s;
    uint8_t sn[16], plain[16], out[300];
    uint16_t out_len = 0;

    fake_reset(); sec_init(0);
    od_session_init(&s, 0);
    CHECK(handshake(&s, 1000u, sn, false) == OD_SESSION_AUTH_ESTABLISHED);
    plain[0] = 0x00u; plain[1] = 0x70u;

    CASE("a successful seal stamps exactly now_ms");
    s.last_activity_ms = 0u;
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, sizeof out, &out_len, 7777u, NULL)
          == OD_SESSION_SEAL_OK);
    CHECK(s.last_activity_ms == 7777u);

    CASE("TOO_SHORT does not stamp");
    s.last_activity_ms = 0u;
    CHECK(od_session_seal(&s, od_span_make(plain, 1), out, sizeof out, &out_len, 8888u, NULL)
          == OD_SESSION_SEAL_TOO_SHORT);
    CHECK(s.last_activity_ms == 0u);

    CASE("TOO_LONG does not stamp");
    {
        uint8_t big[OD_SESSION_PAYLOAD_MAX + 3u];
        memset(big, 0, sizeof big);
        s.last_activity_ms = 0u;
        CHECK(od_session_seal(&s, od_span_make(big, sizeof big), out, sizeof out, &out_len,
                              8888u, NULL) == OD_SESSION_SEAL_TOO_LONG);
        CHECK(s.last_activity_ms == 0u);
    }

    CASE("NO_ROOM does not stamp");
    s.last_activity_ms = 0u;
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, 8u, &out_len, 8888u, NULL)
          == OD_SESSION_SEAL_NO_ROOM);
    CHECK(s.last_activity_ms == 0u);

    CASE("a cipher failure does not stamp, even though it spent a counter");
    {
        const uint64_t before = s.tx_counter;
        s.last_activity_ms = 0u;
        g_force_status = OD_HAL_CRYPTO_ERROR;
        g_force_after = 0;
        CHECK(od_session_seal(&s, od_span_make(plain, 10), out, sizeof out, &out_len, 8888u, NULL)
              == OD_SESSION_SEAL_CRYPTO_ERROR);
        CHECK(s.tx_counter == before + 1u);
        CHECK(s.last_activity_ms == 0u);
        g_force_status = OD_HAL_CRYPTO_OK;
        g_force_after = -1;
    }

    CASE("COUNTER_EXHAUSTED does not stamp");
    s.tx_counter = UINT64_MAX;
    s.last_activity_ms = 0u;
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, sizeof out, &out_len, 8888u, NULL)
          == OD_SESSION_SEAL_COUNTER_EXHAUSTED);
    CHECK(s.last_activity_ms == 0u);

    CASE("NO_SESSION does not stamp");
    od_session_clear(&s);
    s.last_activity_ms = 0u;
    CHECK(od_session_seal(&s, od_span_make(plain, 10), out, sizeof out, &out_len, 8888u, NULL)
          == OD_SESSION_SEAL_NO_SESSION);
    CHECK(s.last_activity_ms == 0u);

    CASE("od_session_touch() still stamps unconditionally");
    od_session_touch(&s, 4242u);
    CHECK(s.last_activity_ms == 4242u);
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
    test_host_cmac_rfc4493();
    test_every_step2_crypto_exit();
    test_all_zero_session_id_rejected();
    test_step2_reply_bytes();
    test_expired_session_refuses_open_and_seal();
    test_security_enabled_is_fail_safe();
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
    test_seal_activity_success_only();

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
