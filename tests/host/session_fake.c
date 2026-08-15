/* session_fake.c -- the fake crypto HAL every od_session host binary links against.
 *
 * Extracted from session_test.c so the libFuzzer targets in tests/fuzz/ drive the SAME fake the
 * unit suite does. A second, subtly different fake would make a fuzz finding unreproducible in
 * the suite, which is where findings have to land to stay fixed.
 *
 * WHAT A FAKE HAL CAN AND CANNOT PROVE. It proves the POLICY: the handshake state machine, the
 * KDF chain, the replay window, the strike and lockout rules, the envelope framing. It cannot
 * prove that mbedTLS or PSA is driven correctly, that a PSA key policy is right, or that the tag
 * a real backend produces matches this one -- those need the targets, and C5/C6 hardware. Do not
 * let a green run here be quoted as "the session works".
 */

#include "session_fake.h"

#include "aes128.h"

#include <string.h>

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

void host_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t out[16])
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

uint8_t  g_slot_key[OD_HAL_CRYPTO_KEY_SLOTS][16];
static bool     g_slot_loaded[OD_HAL_CRYPTO_KEY_SLOTS];
unsigned g_key_set_calls;
unsigned g_key_clear_calls;
static uint8_t  g_rand_next;                    /* deterministic: handshakes must reproduce */
enum od_hal_crypto_status g_force_status; /* OK, or an error to inject */

void fake_reset(void)
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

const uint8_t MASTER[16] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
};
const uint8_t DEVICE_ID[4] = { 0xDE,0xAD,0xBE,0xEF };

struct SecurityConfig g_sec;

void sec_init(uint16_t timeout_s)
{
    memset(&g_sec, 0, sizeof g_sec);
    g_sec.encryption_enabled = 1u;
    memcpy(g_sec.encryption_key, MASTER, 16u);
    g_sec.session_timeout_seconds = timeout_s;
}

bool handshake_step1(struct od_session *s, uint32_t now_ms, uint8_t server_nonce_out[16],
                     uint8_t *rsp, uint16_t *rsp_len)
{
    uint8_t body1[1] = { 0x00 };

    if (od_session_authenticate(s, &g_sec, DEVICE_ID, od_span_make(body1, 1), now_ms,
                                rsp, OD_SESSION_REPLY_MAX, rsp_len, NULL)
        != OD_SESSION_AUTH_CHALLENGE) {
        return false;
    }
    memcpy(server_nonce_out, rsp + 3, 16u);
    return true;
}

/* Drive a full handshake. Returns the od_session_auth of step 2 and, on success, leaves the
 * session open. server_nonce_out receives the challenge so tests can recompute proofs. */
enum od_session_auth handshake(struct od_session *s, uint32_t now_ms,
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
