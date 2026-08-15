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

/* PLACEHOLDER BODIES. C4 fills these; the names are the contract this file already commits to,
 * taken from the plan's "cases that carry the weight" list so none is quietly dropped. Each one
 * that is still empty CHECKs false, so an unfinished suite cannot read as green. */
#define TODO_C4() CHECK(!"case not written yet (C4)")

static void test_handshake_challenge_then_proof(void) { CASE("step 1 then step 2 opens a session"); TODO_C4(); }
static void test_wrong_proof_rejected(void)           { CASE("wrong proof: REJECTED, no session"); TODO_C4(); }
static void test_kdf_matches_transcription(void)      { CASE("KDF agrees with the shipped chain"); TODO_C4(); }
static void test_challenge_expiry_boundary(void)      { CASE("challenge at 29999/30000 accepted, 30001 expired"); TODO_C4(); }
static void test_rate_limit(void)                     { CASE("10 attempts in 60 s, and it survives a clear"); TODO_C4(); }
static void test_rate_limited_keeps_challenge(void)   { CASE("RATE_LIMITED does not consume the challenge"); TODO_C4(); }
static void test_step1_over_live_session(void)        { CASE("step 1 over a live session re-challenges"); TODO_C4(); }
static void test_capacity_is_transactional(void)      { CASE("rsp_cap 0..22: NO_ROOM and no state change"); TODO_C4(); }
static void test_clock_starting_at_zero(void)         { CASE("now_ms == 0 is a real time"); TODO_C4(); }

static void test_replay_bitmap_sweep(void)            { CASE("bitmap vs a brute-force seen-set model"); TODO_C4(); }
static void test_replay_exact_counter(void)           { CASE("diff == 0 refused, and the ring oracle accepted it"); TODO_C4(); }
static void test_replay_counter_zero_backfill(void)   { CASE("counter 0 backfill: bitmap accepts, ring did not"); TODO_C4(); }
static void test_replay_window_boundaries(void)       { CASE("+32/+33 and -32/-33"); TODO_C4(); }
static void test_window_not_advanced_on_bad_tag(void) { CASE("bad tag leaves rx_last unmoved"); TODO_C4(); }

static void test_envelope_roundtrip(void)             { CASE("seal then open returns the payload"); TODO_C4(); }
static void test_envelope_min_and_max(void)           { CASE("28 SHORT, 29 ok, 251 ok, 252 TOO_LONG"); TODO_C4(); }
static void test_inner_length_exact(void)             { CASE("declared length off by one either way is BAD_LENGTH"); TODO_C4(); }
static void test_seal_needs_cmd_bytes(void)           { CASE("plain_frame < 2 is TOO_SHORT"); TODO_C4(); }
static void test_seal_no_room_burns_nothing(void)     { CASE("NO_ROOM leaves tx_counter unmoved"); TODO_C4(); }
static void test_tx_counter_exhaustion(void)          { CASE("UINT64_MAX: COUNTER_EXHAUSTED, never counter 0"); TODO_C4(); }

static void test_timeout_absolute_and_wrap_safe(void) { CASE("expires on age across the uint32 rollover"); TODO_C4(); }
static void test_timeout_boundary(void)               { CASE("exactly timeout_ms expires, minus one does not"); TODO_C4(); }
static void test_integrity_strikes(void)              { CASE("3 bad tags tear down, 3 crypto errors do not"); TODO_C4(); }
static void test_slot_preserved_across_clear(void)    { CASE("clear preserves s->slot"); TODO_C4(); }
static void test_no_slot_leak(void)                   { CASE("1000 re-auths leak no slot"); TODO_C4(); }
static void test_null_safety(void)                    { CASE("every entry point tolerates NULL"); TODO_C4(); }

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

    printf("session: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
