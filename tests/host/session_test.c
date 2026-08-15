/* session_test.c -- shared/core/od_session.c against a fake crypto HAL.
 *
 * NOT REGISTERED in CMakeLists.txt yet. It is written against od_session.h before od_session.c
 * exists (the od_adv_control precedent) and compiles today, but cannot link until C4 lands the
 * implementation. Registering a test with nothing to link is a red CI, not a discipline.
 *
 * WHAT A FAKE HAL CAN AND CANNOT PROVE. It proves the POLICY: the handshake state machine, the
 * KDF chain, the replay window, the strike and lockout rules, the envelope framing. It cannot
 * prove that mbedTLS or PSA is driven correctly, that a PSA key policy is right, or that the tag
 * a real backend produces matches this one -- those need the targets, and C5/C6 hardware. Do not
 * let a green run here be quoted as "the session works".
 *
 * AND WITH THE C0 CAPTURE SKIPPED, THE REFERENCE IS A TRANSCRIPTION. The differential layers
 * below compare od_session.c against code transcribed from the shipped targets, not against bytes
 * a device emitted. A misreading of the shipped code is therefore invisible: both sides are wrong
 * the same way. Hardware at C5/C6 is the only check that the wire did not move.
 */

#include "od_session.h"

#include "aes128.h"

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

/* --------------------------------------------------------------------- AES-CMAC on the host --- */

/* Left-shift a 16-byte block by one bit, XORing the CMAC constant on overflow (RFC 4493 §2.3). */
static void cmac_dbl(uint8_t b[16])
{
    uint8_t carry = (uint8_t)(b[0] >> 7);
    unsigned i;

    for (i = 0; i < 15u; ++i) {
        b[i] = (uint8_t)((b[i] << 1) | (b[i + 1u] >> 7));
    }
    b[15] = (uint8_t)(b[15] << 1);
    if (carry) {
        b[15] ^= 0x87u;
    }
}

static void host_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t out[16])
{
    uint8_t k1[16], k2[16], x[16], blk[16];
    uint32_t full, i;

    memset(x, 0, 16u);
    od_test_aes128_encrypt(key, x, k1);
    cmac_dbl(k1);
    memcpy(k2, k1, 16u);
    cmac_dbl(k2);

    full = (len == 0u) ? 0u : ((len - 1u) / 16u);   /* complete blocks before the last */
    memset(x, 0, 16u);
    for (i = 0; i < full; ++i) {
        unsigned j;
        for (j = 0; j < 16u; ++j) { blk[j] = (uint8_t)(x[j] ^ msg[i * 16u + j]); }
        od_test_aes128_encrypt(key, blk, x);
    }

    {
        uint32_t rem = len - (full * 16u);
        unsigned j;
        if (len != 0u && rem == 16u) {
            for (j = 0; j < 16u; ++j) { blk[j] = (uint8_t)(msg[full * 16u + j] ^ k1[j]); }
        } else {
            memset(blk, 0, 16u);
            if (rem > 0u) { memcpy(blk, &msg[full * 16u], rem); }
            blk[rem] = 0x80u;
            for (j = 0; j < 16u; ++j) { blk[j] ^= k2[j]; }
        }
        for (j = 0; j < 16u; ++j) { blk[j] ^= x[j]; }
        od_test_aes128_encrypt(key, blk, out);
    }
}

/* ------------------------------------------------------- the preserved soft-CCM reference --- */

/* session_ccm_reference.inc is the RFC 3610 implementation targets/nordic-zephyr shipped, copied
 * verbatim before C1 deleted it. It is not a translation unit: it needs these two symbols, which
 * are the only things it took from its target. Supplying them here is what makes the shipped
 * algorithm usable as an independent oracle. */
static uint8_t g_ref_key[16];
static struct { uint8_t *session_key; } s_session = { g_ref_key };

static bool aes_ecb_encrypt_16(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    od_test_aes128_encrypt(key, in, out);
    return true;
}

#include "session_ccm_reference.inc"

/* ------------------------------------------------------------------------------ fake HAL --- */

/* Defined at file scope and bound at LINK time, exactly as a target does -- shared/ binds its HAL
 * at link time by design, so there is no injection seam and this exercises real linkage. */

static uint8_t  g_slot_key[OD_HAL_CRYPTO_KEY_SLOTS][16];
static bool     g_slot_loaded[OD_HAL_CRYPTO_KEY_SLOTS];
static unsigned g_key_set_calls;
static unsigned g_key_clear_calls;
static uint8_t  g_rand_next;                    /* deterministic: handshakes must reproduce */
static enum od_hal_crypto_status g_force_status; /* OK, or an error to inject */

static void fake_reset(void)
{
    memset(g_slot_key, 0, sizeof g_slot_key);
    memset(g_slot_loaded, 0, sizeof g_slot_loaded);
    g_key_set_calls = 0;
    g_key_clear_calls = 0;
    g_rand_next = 0;
    g_force_status = OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot, const uint8_t key[16])
{
    if (g_force_status != OD_HAL_CRYPTO_OK) { return g_force_status; }
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS) { return OD_HAL_CRYPTO_ERROR; }
    ++g_key_set_calls;
    memcpy(g_slot_key[slot], key, 16u);
    g_slot_loaded[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS) { return; }
    ++g_key_clear_calls;
    memset(g_slot_key[slot], 0, 16u);
    g_slot_loaded[slot] = false;
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    if (g_force_status != OD_HAL_CRYPTO_OK) { return g_force_status; }
    host_cmac(key, msg, msg_len, out);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    if (g_force_status != OD_HAL_CRYPTO_OK) { return g_force_status; }
    od_test_aes128_encrypt(key, in, out);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (g_force_status != OD_HAL_CRYPTO_OK) { return g_force_status; }
    for (i = 0; i < len; ++i) { buf[i] = g_rand_next++; }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len)
{
    if (g_force_status != OD_HAL_CRYPTO_OK) { return g_force_status; }
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || !g_slot_loaded[slot]) { return OD_HAL_CRYPTO_ERROR; }
    if (nonce_len != OD_SESSION_CCM_NONCE_LEN || aad_len != 2u) { return OD_HAL_CRYPTO_ERROR; }
    if (ct_cap < (uint16_t)(plain_len + OD_HAL_CRYPTO_TAG_LEN)) { return OD_HAL_CRYPTO_ERROR; }

    memcpy(g_ref_key, g_slot_key[slot], 16u);
    if (!od_ccm_encrypt(nonce, aad, plain, plain_len, ct, &ct[plain_len])) {
        return OD_HAL_CRYPTO_ERROR;
    }
    *ct_len = (uint16_t)(plain_len + OD_HAL_CRYPTO_TAG_LEN);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len)
{
    uint16_t body;

    if (g_force_status != OD_HAL_CRYPTO_OK) { return g_force_status; }
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || !g_slot_loaded[slot]) { return OD_HAL_CRYPTO_ERROR; }
    if (nonce_len != OD_SESSION_CCM_NONCE_LEN || aad_len != 2u) { return OD_HAL_CRYPTO_ERROR; }
    if (ct_len <= OD_HAL_CRYPTO_TAG_LEN) { return OD_HAL_CRYPTO_ERROR; }
    body = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    if (plain_cap < body) { return OD_HAL_CRYPTO_ERROR; }

    memcpy(g_ref_key, g_slot_key[slot], 16u);
    if (!od_ccm_decrypt(nonce, aad, ct, body, &ct[body], plain)) {
        return OD_HAL_CRYPTO_AUTH_FAILED;   /* the reference reports only tag failure this way */
    }
    *plain_len = body;
    return OD_HAL_CRYPTO_OK;
}

/* --------------------------------------------------------------------------------- cases --- */

static const uint8_t MASTER[16] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
};
static const uint8_t DEVICE_ID[4] = { 0xDE,0xAD,0xBE,0xEF };

static struct SecurityConfig g_sec;

static void sec_init(uint16_t timeout_s)
{
    memset(&g_sec, 0, sizeof g_sec);
    g_sec.encryption_enabled = 1u;
    memcpy(g_sec.encryption_key, MASTER, 16u);
    g_sec.session_timeout_seconds = timeout_s;
}

/* Drive a full handshake. Returns the od_session_auth of step 2 and, on success, leaves the
 * session open. server_nonce_out receives the challenge so tests can recompute proofs. */
static enum od_session_auth handshake(struct od_session *s, uint32_t now_ms,
                                      uint8_t server_nonce_out[16], bool corrupt_proof)
{
    uint8_t rsp[OD_SESSION_REPLY_MAX];
    uint16_t rsp_len = 0;
    uint8_t body1[1] = { 0x00 };
    uint8_t body2[32];
    static const uint8_t CLIENT_NONCE[16] = {
        0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf
    };
    uint8_t proof_in[36], proof[16];
    enum od_session_auth r;

    r = od_session_authenticate(s, &g_sec, DEVICE_ID, od_span_make(body1, 1), now_ms,
                                rsp, sizeof rsp, &rsp_len, NULL);
    if (r != OD_SESSION_AUTH_CHALLENGE) { return r; }
    memcpy(server_nonce_out, rsp + 3, 16u);

    memcpy(proof_in, server_nonce_out, 16u);
    memcpy(proof_in + 16, CLIENT_NONCE, 16u);
    memcpy(proof_in + 32, DEVICE_ID, 4u);
    host_cmac(MASTER, proof_in, 36u, proof);
    if (corrupt_proof) { proof[15] ^= 0x01u; }

    memcpy(body2, CLIENT_NONCE, 16u);
    memcpy(body2 + 16, proof, 16u);
    return od_session_authenticate(s, &g_sec, DEVICE_ID, od_span_make(body2, 32), now_ms,
                                   rsp, sizeof rsp, &rsp_len, NULL);
}

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
