/* od_session.c -- see od_session.h. Plain C99, no HAL beyond od_hal_crypto, no allocation,
 * no logging.
 *
 * Written against ../Firmware/src/encryption.cpp @ 64184bb -- the sibling repo, NOT this repo's
 * targets/esp32-idf/src/ snapshot, which is older (CLAUDE.md, "Migration constraints"). The
 * replay window is shared/core/od_nonce_window.h, a verbatim port of upstream's.
 */

#include "od_session.h"

#include "od_config.h"
#include "od_nonce_window.h"

#include <string.h>

/* ------------------------------------------------------------------------------ primitives --- */

/* Not memset: a compiler may drop a dead store to a local going out of scope, which is exactly
 * what this is. The volatile pointer keeps the write observable. */
static void od_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;

    while (n-- != 0u) {
        *v++ = 0u;
    }
}

/* Runs over attacker-supplied bytes; an early exit leaks how many led.
 *
 * The accumulator is volatile so the optimiser may not reason about its value and short-circuit
 * the loop once it is non-zero. The loop bound is data-independent either way, but "the compiler
 * has no reason to do that today" is not a property to rest a timing argument on. This is
 * best-effort portable constant-time code, not a guarantee ISO C offers -- verifying it means
 * reading the optimised target assembly at the 8-byte session-id and 16-byte proof call sites. */
static bool od_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t diff = 0u;
    size_t i;

    for (i = 0; i < len; ++i) {
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    }
    return diff == 0u;
}

static void od_be64(uint8_t out[8], uint64_t v)
{
    unsigned i;

    for (i = 0; i < 8u; ++i) {
        out[i] = (uint8_t)((v >> (56u - (i * 8u))) & 0xFFu);
    }
}

static uint64_t od_rd_be64(const uint8_t in[8])
{
    uint64_t v = 0u;
    unsigned i;

    for (i = 0; i < 8u; ++i) {
        v = (v << 8) | (uint64_t)in[i];
    }
    return v;
}

static void report_reset(struct od_session_report *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

/* ------------------------------------------------------------------------ the KDF, exactly --- */

/* CMAC(master, "OpenDisplay session" ‖ 0x00 ‖ device_id ‖ client ‖ server ‖ 0x00 0x80) -> 58 bytes
 * in, then AES-ECB(master, BE64(1) ‖ intermediate[0..7]).
 *
 * A NIST SP 800-108-flavoured chain: counter 1 big-endian in the FIRST eight bytes, only the
 * first HALF of the CMAC output in the second. Every byte of this is wire-visible through the
 * session key, so it is transcribed rather than tidied. */
#define OD_KDF_LABEL     "OpenDisplay session"
#define OD_KDF_LABEL_LEN 19u
#define OD_KDF_INPUT_LEN (OD_KDF_LABEL_LEN + 1u + OD_SESSION_DEVICE_ID_LEN \
                          + OD_SESSION_NONCE_LEN + OD_SESSION_NONCE_LEN + 2u)
OD_STATIC_ASSERT(OD_KDF_INPUT_LEN == 58u, "the KDF input is 58 bytes");

/* Returns the HAL status rather than a bool: a tag/policy rejection and an engine fault are
 * different diagnoses, and collapsing them here is what made every CRYPTO_ERROR report OK. */
static enum od_hal_crypto_status derive_session_key(const uint8_t master[16],
                               const uint8_t client_nonce[16],
                               const uint8_t server_nonce[16],
                               const uint8_t device_id[OD_SESSION_DEVICE_ID_LEN],
                               uint8_t session_key[16])
{
    uint8_t in[OD_KDF_INPUT_LEN];
    uint8_t intermediate[16];
    uint8_t final_in[16];
    size_t off = 0;
    enum od_hal_crypto_status cs;

    memcpy(in + off, OD_KDF_LABEL, OD_KDF_LABEL_LEN); off += OD_KDF_LABEL_LEN;
    in[off++] = 0x00u;
    memcpy(in + off, device_id, OD_SESSION_DEVICE_ID_LEN); off += OD_SESSION_DEVICE_ID_LEN;
    memcpy(in + off, client_nonce, OD_SESSION_NONCE_LEN); off += OD_SESSION_NONCE_LEN;
    memcpy(in + off, server_nonce, OD_SESSION_NONCE_LEN); off += OD_SESSION_NONCE_LEN;
    in[off++] = 0x00u;
    in[off++] = 0x80u;

    cs = od_hal_crypto_cmac(master, in, (uint32_t)off, intermediate);
    if (cs != OD_HAL_CRYPTO_OK) {
        goto cleanup;
    }
    od_be64(final_in, 1u);
    memcpy(final_in + 8, intermediate, 8u);
    cs = od_hal_crypto_aes_ecb(master, final_in, session_key);

cleanup:
    od_secure_zero(in, sizeof in);
    od_secure_zero(intermediate, sizeof intermediate);
    od_secure_zero(final_in, sizeof final_in);
    return cs;
}

static enum od_hal_crypto_status derive_session_id(const uint8_t session_key[16],
                              const uint8_t client_nonce[16],
                              const uint8_t server_nonce[16], uint8_t session_id[8])
{
    uint8_t in[32];
    uint8_t mac[16];
    enum od_hal_crypto_status cs;

    memcpy(in, client_nonce, 16u);
    memcpy(in + 16, server_nonce, 16u);
    cs = od_hal_crypto_cmac(session_key, in, 32u, mac);
    if (cs != OD_HAL_CRYPTO_OK) {
        memset(session_id, 0, OD_SESSION_ID_LEN);   /* the caller rejects an all-zero id */
        goto cleanup;
    }
    memcpy(session_id, mac, OD_SESSION_ID_LEN);

cleanup:
    od_secure_zero(mac, sizeof mac);
    return cs;
}

/* The proof both directions compute, over nonce_a ‖ nonce_b ‖ device_id. The client's is keyed
 * with the MASTER key and the server's with the SESSION key, which is the only difference. */
static enum od_hal_crypto_status derive_proof(const uint8_t key[16], const uint8_t nonce_a[16],
                         const uint8_t nonce_b[16],
                         const uint8_t device_id[OD_SESSION_DEVICE_ID_LEN], uint8_t out[16])
{
    uint8_t in[36];
    enum od_hal_crypto_status cs;

    memcpy(in, nonce_a, 16u);
    memcpy(in + 16, nonce_b, 16u);
    memcpy(in + 32, device_id, OD_SESSION_DEVICE_ID_LEN);
    cs = od_hal_crypto_cmac(key, in, 36u, out);
    od_secure_zero(in, sizeof in);
    return cs;
}

/* ------------------------------------------------------------------------------- lifecycle --- */

void od_session_init(struct od_session *s, od_hal_crypto_slot_t slot)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->slot = slot;
}

void od_session_clear(struct od_session *s)
{
    od_hal_crypto_slot_t slot;

    if (s == NULL) {
        return;
    }
    slot = s->slot;                       /* configuration, not session state */
    if (s->key_loaded) {
        od_hal_crypto_key_clear(slot);
    }
    od_secure_zero(s, sizeof(*s));
    s->slot = slot;
}

/* The device's OWN outbound counter is zeroed here with the inbound state. A device that carried
 * it across a re-auth while the client restarts at 0 would reuse a keystream against itself. */
static void reset_nonce_state(struct od_session *s)
{
    s->tx_counter = 0u;
    s->rx_last = 0u;
    s->integrity_failures = 0u;
    memset(s->rx_seen, 0, sizeof s->rx_seen);
}

bool od_session_security_enabled(const struct SecurityConfig *sec)
{
    if (sec == NULL) {
        return false;
    }
    /* Nordic's fail-safe reading, not Firmware's `== 1`: a corrupted byte must not silently turn
     * encryption OFF. The zero-key half is od_config's, applied once for every target. */
    return (sec->encryption_enabled != 0u) && od_config_security_key_set(sec);
}

bool od_session_authenticated(const struct od_session *s)
{
    return (s != NULL) && s->authenticated;
}

bool od_session_alive(struct od_session *s, uint32_t now_ms, struct od_session_report *report)
{
    uint32_t age;

    if (s == NULL || !s->authenticated) {
        return false;
    }
    if (s->timeout_ms == 0u) {
        return true;
    }
    age = now_ms - s->session_start_ms;   /* unsigned: correct across the 32-bit rollover */
    if (report != NULL) {
        report->age_ms = age;
    }
    if (age >= s->timeout_ms) {
        od_session_clear(s);
        if (report != NULL) {
            report->torn_down = true;
        }
        return false;
    }
    return true;
}

void od_session_touch(struct od_session *s, uint32_t now_ms)
{
    if (s != NULL) {
        s->last_activity_ms = now_ms;
    }
}

/* --------------------------------------------------------------------------- the handshake --- */

static enum od_session_auth auth_fail(uint8_t status, uint8_t *rsp, uint16_t *rsp_len,
                                      struct od_session_report *report,
                                      enum od_session_auth result)
{
    rsp[0] = RESP_ACK;
    rsp[1] = RESP_AUTHENTICATE;
    rsp[2] = status;
    *rsp_len = 3u;
    if (report != NULL) {
        report->status_byte = status;
    }
    return result;
}

/* CRYPTO_ERROR with the HAL's own status attached. Without this every engine failure reported
 * OD_HAL_CRYPTO_OK (enum zero, what report_reset leaves behind), which is exactly the evidence
 * needed to tell a PSA key-policy rejection from a generic fault on first hardware. */
static enum od_session_auth auth_crypto_fail(enum od_hal_crypto_status cs, uint8_t *rsp,
                                             uint16_t *rsp_len, struct od_session_report *report)
{
    if (report != NULL) {
        report->crypto_status = cs;
    }
    return auth_fail(AUTH_STATUS_ERROR, rsp, rsp_len, report, OD_SESSION_AUTH_CRYPTO_ERROR);
}

/* ONE CHALLENGE ANSWERS ONE STEP 2. Consumed by every step-2 outcome that reaches the request
 * shape -- success, wrong proof, expiry, malformed body, and any crypto failure -- so an attacker
 * cannot grind proofs against a fixed server nonce. Deliberately NOT called by RATE_LIMITED,
 * BAD_ARGUMENT or the capacity preflight: all three return before the request is examined, and a
 * throttled client must still be able to answer the challenge it already holds. */
static void challenge_consume(struct od_session *s)
{
    s->challenge_pending = false;
    s->challenge_ms = 0u;
    od_secure_zero(s->pending_server_nonce, OD_SESSION_NONCE_LEN);
}

enum od_session_auth od_session_authenticate(struct od_session *s,
        const struct SecurityConfig *sec,
        const uint8_t device_id[OD_SESSION_DEVICE_ID_LEN],
        od_span_t body, uint32_t now_ms,
        uint8_t *rsp, size_t rsp_cap, uint16_t *rsp_len,
        struct od_session_report *report)
{
    bool is_step1;
    bool is_step2;
    enum od_hal_crypto_status cs;

    report_reset(report);

    if (s == NULL || device_id == NULL || rsp == NULL || rsp_len == NULL) {
        return OD_SESSION_AUTH_BAD_ARGUMENT;
    }
    *rsp_len = 0u;
    /* Every reply is at least three bytes, so this is the floor for saying anything at all. */
    if (rsp_cap < 3u) {
        return OD_SESSION_AUTH_NO_ROOM;
    }
    if (!od_session_security_enabled(sec)) {
        return auth_fail(AUTH_STATUS_NOT_CONFIG, rsp, rsp_len, report,
                         OD_SESSION_AUTH_NOT_CONFIGURED);
    }

    /* CAPACITY PREFLIGHT AHEAD OF THE RATE LIMITER. Classifying the body only READS it, and
     * rsp_cap is a caller-local property no peer can influence, so nothing peer-observable moves:
     * what this buys is that a NO_ROOM return cannot consume an attempt or open the rate window,
     * which is what "capacity is checked before any state change" in the header actually claims. */
    is_step1 = (od_span_has(body, 1u) && body.n == 1u && body.p[0] == 0x00u);
    is_step2 = (od_span_has(body, OD_SESSION_STEP2_BODY_LEN) &&
                body.n == OD_SESSION_STEP2_BODY_LEN);
    if (is_step1 && rsp_cap < OD_SESSION_STEP1_REPLY_LEN) {
        return OD_SESSION_AUTH_NO_ROOM;
    }
    if (is_step2 && rsp_cap < OD_SESSION_STEP2_REPLY_LEN) {
        return OD_SESSION_AUTH_NO_ROOM;
    }

    /* RATE LIMIT next, before the request shape is ACTED ON -- the shipped order. The window
     * resets on a 60 s idle gap rather than 60 s from the first attempt, because last_auth_ms is
     * rewritten by every ACCEPTED attempt. A THROTTLED one returns below without updating it, so
     * the window does drain while a peer keeps knocking: at 59 s pacing the 11th attempt is
     * refused and the 12th is accepted again. That matches upstream (encryption.cpp:602-615,
     * which also returns before its update) and is the behaviour to keep -- making the refusal
     * extend the window would let any unauthenticated peer send one 1-byte 0x0050 per 59 s and
     * lock the legitimate client out of re-authentication for good.
     * auth_attempts == 0 means "no window open", which is why there is no separate flag. */
    if (s->auth_attempts != 0u && (now_ms - s->last_auth_ms) < OD_SESSION_RATE_WINDOW_MS) {
        if (s->auth_attempts >= OD_SESSION_RATE_MAX_ATTEMPTS) {
            if (report != NULL) {
                report->attempts = s->auth_attempts;
            }
            /* Does NOT consume a pending challenge: a throttled client must still be able to
             * answer the one it holds once the window clears. */
            return auth_fail(AUTH_STATUS_RATE_LIMIT, rsp, rsp_len, report,
                             OD_SESSION_AUTH_RATE_LIMITED);
        }
    } else if (s->auth_attempts != 0u) {
        s->auth_attempts = 0u;            /* the idle gap elapsed */
    }
    if (s->auth_attempts < 0xFFu) {
        s->auth_attempts++;
    }
    s->last_auth_ms = now_ms;
    if (report != NULL) {
        report->attempts = s->auth_attempts;
    }

    /* ---- STEP 1: issue a challenge ---- */
    if (is_step1) {
        /* A step 1 over a live session replaces it. AUTH_STATUS_ALREADY exists in the protocol
         * and is deliberately never sent; re-authentication is the normal recovery path. */
        if (s->authenticated) {
            od_session_clear(s);
        }
        cs = od_hal_crypto_random(s->pending_server_nonce, OD_SESSION_NONCE_LEN);
        if (cs != OD_HAL_CRYPTO_OK) {
            /* No challenge is outstanding: never offer one the device cannot honour. */
            challenge_consume(s);
            return auth_crypto_fail(cs, rsp, rsp_len, report);
        }
        s->challenge_ms = now_ms;
        s->challenge_pending = true;             /* set only AFTER the RNG succeeded */

        rsp[0] = RESP_ACK;
        rsp[1] = RESP_AUTHENTICATE;
        rsp[2] = AUTH_STATUS_CHALLENGE;
        memcpy(rsp + 3, s->pending_server_nonce, OD_SESSION_NONCE_LEN);
        memcpy(rsp + 19, device_id, OD_SESSION_DEVICE_ID_LEN);
        *rsp_len = OD_SESSION_STEP1_REPLY_LEN;
        if (report != NULL) {
            report->status_byte = AUTH_STATUS_CHALLENGE;
        }
        return OD_SESSION_AUTH_CHALLENGE;
    }

    /* ---- STEP 2: verify the proof ---- */
    if (is_step2) {
        const uint8_t *client_nonce = body.p;
        const uint8_t *client_proof = body.p + OD_SESSION_NONCE_LEN;
        uint8_t expected[16];
        uint8_t session_key[16];
        uint8_t proof[16];
        enum od_session_auth result;
        unsigned i;
        bool id_ok;

        if (!s->challenge_pending) {
            return auth_fail(AUTH_STATUS_ERROR, rsp, rsp_len, report, OD_SESSION_AUTH_MALFORMED);
        }
        /* `>` and not `>=`: exactly OD_SESSION_CHALLENGE_WINDOW_MS is still accepted. This is the
         * opposite convention from the session timeout above, and both match what shipped. */
        if ((now_ms - s->challenge_ms) > OD_SESSION_CHALLENGE_WINDOW_MS) {
            challenge_consume(s);
            return auth_fail(AUTH_STATUS_ERROR, rsp, rsp_len, report, OD_SESSION_AUTH_EXPIRED);
        }

        cs = derive_proof(sec->encryption_key, s->pending_server_nonce, client_nonce,
                          device_id, expected);
        if (cs != OD_HAL_CRYPTO_OK) {
            result = auth_crypto_fail(cs, rsp, rsp_len, report);
            goto step2_cleanup;
        }
        if (!od_ct_equal(client_proof, expected, OD_SESSION_MAC_LEN)) {
            result = auth_fail(AUTH_STATUS_FAILED, rsp, rsp_len, report,
                               OD_SESSION_AUTH_REJECTED);
            goto step2_cleanup;
        }

        cs = derive_session_key(sec->encryption_key, client_nonce, s->pending_server_nonce,
                                device_id, session_key);
        if (cs != OD_HAL_CRYPTO_OK) {
            result = auth_crypto_fail(cs, rsp, rsp_len, report);
            goto step2_cleanup;
        }
        cs = derive_session_id(session_key, client_nonce, s->pending_server_nonce,
                               s->session_id);
        if (cs != OD_HAL_CRYPTO_OK) {
            result = auth_crypto_fail(cs, rsp, rsp_len, report);
            goto step2_cleanup;
        }
        /* An all-zero session id would make every nonce collide with a cleared session. */
        id_ok = false;
        for (i = 0; i < OD_SESSION_ID_LEN; ++i) {
            if (s->session_id[i] != 0u) { id_ok = true; break; }
        }
        if (!id_ok) {
            result = auth_crypto_fail(OD_HAL_CRYPTO_ERROR, rsp, rsp_len, report);
            goto step2_cleanup;
        }

        /* BOTH proofs take the nonces server-first; the ONLY difference is the key -- master for
         * the client's, session for the device's. Verified against Firmware/src/encryption.cpp
         * :647-649 and :703-705 and against py-opendisplay compute_server_proof(). Do not "fix"
         * this to mirror the argument order: swapping it makes every deployed host fail
         * AuthenticationError, and it is wire-visible, so no host update can rescue it. */
        cs = derive_proof(session_key, s->pending_server_nonce, client_nonce, device_id, proof);
        if (cs != OD_HAL_CRYPTO_OK) {
            od_session_clear(s);
            result = auth_crypto_fail(cs, rsp, rsp_len, report);
            goto step2_cleanup;
        }
        cs = od_hal_crypto_key_set(s->slot, session_key);
        if (cs != OD_HAL_CRYPTO_OK) {
            od_session_clear(s);
            result = auth_crypto_fail(cs, rsp, rsp_len, report);
            goto step2_cleanup;
        }

        s->key_loaded = true;
        s->authenticated = true;
        reset_nonce_state(s);
        s->timeout_ms = (uint32_t)sec->session_timeout_seconds * 1000u;
        s->session_start_ms = now_ms;
        s->last_activity_ms = now_ms;
        challenge_consume(s);
        s->auth_attempts = 0u;            /* a good handshake ends the run */

        rsp[0] = RESP_ACK;
        rsp[1] = RESP_AUTHENTICATE;
        rsp[2] = AUTH_STATUS_SUCCESS;
        memcpy(rsp + 3, proof, OD_SESSION_MAC_LEN);
        *rsp_len = OD_SESSION_STEP2_REPLY_LEN;
        if (report != NULL) {
            report->status_byte = AUTH_STATUS_SUCCESS;
        }
        result = OD_SESSION_AUTH_ESTABLISHED;

step2_cleanup:
        /* Every step-2 path that reached the proof check spends the challenge, whatever the
         * outcome. Idempotent, so the success and wrong-proof paths above may also call it. */
        challenge_consume(s);
        od_secure_zero(expected, sizeof expected);
        od_secure_zero(session_key, sizeof session_key);
        od_secure_zero(proof, sizeof proof);
        return result;
    }

    /* Neither shape. A pending challenge is spent here too: the plan lists a malformed body among
     * the consuming outcomes, and a garbage frame from a client holding a challenge is exactly
     * the case where leaving it alive serves nobody. No-op when none is pending. */
    challenge_consume(s);
    return auth_fail(AUTH_STATUS_ERROR, rsp, rsp_len, report, OD_SESSION_AUTH_MALFORMED);
}

/* ----------------------------------------------------------------------------- the envelope --- */

/* nonce = session_id(8) ‖ BE64(counter). The CCM nonce is bytes [3..15] of it. */
static void build_nonce(const struct od_session *s, uint64_t counter, uint8_t out[16])
{
    memcpy(out, s->session_id, OD_SESSION_ID_LEN);
    od_be64(out + OD_SESSION_ID_LEN, counter);
}

static void strike(struct od_session *s, struct od_session_report *report)
{
    if (s->integrity_failures < 0xFFu) {
        s->integrity_failures++;
    }
    if (report != NULL) {
        report->integrity_failures = s->integrity_failures;
    }
    if (s->integrity_failures >= OD_SESSION_INTEGRITY_STRIKES) {
        od_session_clear(s);
        if (report != NULL) {
            report->torn_down = true;
        }
    }
}

enum od_session_open od_session_open(struct od_session *s, uint16_t cmd, od_span_t envelope,
        uint8_t *out, size_t out_cap, uint16_t *out_len,
        uint32_t now_ms, struct od_session_report *report)
{
    uint8_t nonce[OD_SESSION_NONCE_LEN];
    uint8_t aad[2];
    uint64_t counter;
    uint16_t ct_len;
    uint16_t plain_len = 0u;
    enum NonceResult nr;
    enum od_hal_crypto_status cs;

    report_reset(report);

    if (s == NULL || out == NULL || out_len == NULL || !od_span_valid(envelope)) {
        return OD_SESSION_OPEN_SHORT;
    }
    *out_len = 0u;
    if (!od_session_alive(s, now_ms, report)) {
        return OD_SESSION_OPEN_NO_SESSION;
    }
    /* Length first, before any subtraction: an under-length frame is where a subtraction turns
     * into a huge unsigned length. */
    if (envelope.n < OD_SESSION_ENVELOPE_MIN) {
        return OD_SESSION_OPEN_SHORT;
    }
    if (envelope.n > OD_SESSION_ENVELOPE_MAX) {
        return OD_SESSION_OPEN_TOO_LONG;
    }

    memcpy(nonce, envelope.p, OD_SESSION_NONCE_LEN);
    ct_len = (uint16_t)(envelope.n - OD_SESSION_NONCE_LEN);
    counter = od_rd_be64(nonce + OD_SESSION_ID_LEN);
    if (report != NULL) {
        report->rx_counter = counter;
        report->rx_last = s->rx_last;
        report->rx_diff = (counter >= s->rx_last)
                          ? (int32_t)((counter - s->rx_last) > 0x7FFFFFFFu ? 0x7FFFFFFF
                                      : (int32_t)(counter - s->rx_last))
                          : -(int32_t)((s->rx_last - counter) > 0x7FFFFFFFu ? 0x7FFFFFFF
                                       : (int32_t)(s->rx_last - counter));
    }

    /* The session-id half of the nonce is a nonce rule upstream, not a separate branch. */
    if (!od_ct_equal(nonce, s->session_id, OD_SESSION_ID_LEN)) {
        if (report != NULL) {
            report->nonce_reason = (uint8_t)NONCE_BAD_SESSION;
        }
        /* NOT a strike: a session-id mismatch is what a stale client sends after the device
         * re-authenticated. Only the CCM tag is a tamper oracle. */
        return OD_SESSION_OPEN_WRONG_SESSION;
    }

    /* CHECK is pure. Nothing below may advance the window until the tag verifies. */
    nr = od_nonce_check(s->rx_seen, s->rx_last, counter);
    if (nr != NONCE_OK) {
        if (report != NULL) {
            report->nonce_reason = (uint8_t)nr;
        }
        /* Also not a strike: replay and out-of-window are evidence of a lossy link. Counting them
         * would let ordinary packet loss tear a live session down. */
        return OD_SESSION_OPEN_REPLAY;
    }

    if (out_cap < (size_t)ct_len - OD_HAL_CRYPTO_TAG_LEN) {
        return OD_SESSION_OPEN_NO_ROOM;
    }

    aad[0] = (uint8_t)((cmd >> 8) & 0xFFu);
    aad[1] = (uint8_t)(cmd & 0xFFu);
    cs = od_hal_crypto_ccm_decrypt(s->slot, nonce + 3, OD_SESSION_CCM_NONCE_LEN, aad, 2u,
                                   envelope.p + OD_SESSION_NONCE_LEN, ct_len,
                                   /* The BOUNDED requirement, not the caller's out_cap: casting
                                    * an arbitrary size_t can wrap a large-but-valid buffer down
                                    * to a small capacity and manufacture a crypto error. The
                                    * check above already proved out_cap covers this much. */
                                   out, (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN), &plain_len);
    if (cs == OD_HAL_CRYPTO_AUTH_FAILED) {
        strike(s, report);               /* the tag IS the tamper oracle */
        return OD_SESSION_OPEN_BAD_TAG;
    }
    if (cs != OD_HAL_CRYPTO_OK) {
        if (report != NULL) {
            report->crypto_status = cs;
        }
        return OD_SESSION_OPEN_CRYPTO_ERROR;   /* an engine fault is not an attack */
    }

    /* COMMIT AHEAD OF THE INNER-LENGTH CHECK, deliberately: this frame carried a valid tag, so it
     * is authentic and must be recorded. Committing after the length check would leave an
     * authentic frame replayable. */
    od_nonce_commit(s->rx_seen, &s->rx_last, counter);

    if (plain_len < 1u || (uint16_t)(out[0] + 1u) != plain_len) {
        /* Exact, where upstream is permissive (`payload_length > encrypted_len - 1`). No producer
         * emits trailing bytes, and slack in a length field on this path buys nothing. This is the
         * one deliberate tightening in the promotion -- if a device ever rejects a legitimate
         * frame with BAD_LENGTH, this is the line to look at first. */
        return OD_SESSION_OPEN_BAD_LENGTH;
    }
    *out_len = (uint16_t)(plain_len - 1u);
    if (*out_len > 0u) {
        memmove(out, out + 1, *out_len);
    }
    s->integrity_failures = 0u;
    s->last_activity_ms = now_ms;
    return OD_SESSION_OPEN_OK;
}

enum od_session_seal od_session_seal(struct od_session *s, od_span_t plain_frame,
        uint8_t *out, size_t out_cap, uint16_t *out_len,
        uint32_t now_ms, struct od_session_report *report)
{
    uint8_t nonce[OD_SESSION_NONCE_LEN];
    uint8_t aad[2];
    uint8_t inner[1u + OD_SESSION_PAYLOAD_MAX];
    uint16_t payload_len;
    uint16_t sealed_len;
    uint16_t ct_len = 0u;
    enum od_hal_crypto_status cs;
    enum od_session_seal result;

    report_reset(report);

    if (s == NULL || out == NULL || out_len == NULL || !od_span_valid(plain_frame)) {
        return OD_SESSION_SEAL_NO_ROOM;
    }
    *out_len = 0u;
    if (!od_session_alive(s, now_ms, report)) {
        return OD_SESSION_SEAL_NO_SESSION;
    }
    if (plain_frame.n < 2u) {
        return OD_SESSION_SEAL_TOO_SHORT;      /* no cmd bytes to echo or use as AAD */
    }
    /* Bound the frame in the size_t domain FIRST. Narrowing before the test discards the high
     * bits that make it over-long: plain_frame.n == 65538 becomes payload_len 0, passes the
     * maximum check, and seals an empty payload instead of returning TOO_LONG. No target call
     * site can reach that today -- every one passes a statically bounded frame -- but the span
     * length is a size_t precisely so the public API does not depend on that. */
    if (plain_frame.n > (size_t)OD_SESSION_PLAIN_FRAME_MAX) {
        return OD_SESSION_SEAL_TOO_LONG;
    }
    payload_len = (uint16_t)(plain_frame.n - 2u);
    sealed_len = (uint16_t)(plain_frame.n + OD_SESSION_NONCE_LEN + 1u + OD_HAL_CRYPTO_TAG_LEN);
    /* Preflight: a short output buffer must not burn a counter value. */
    if (out_cap < sealed_len) {
        return OD_SESSION_SEAL_NO_ROOM;
    }
    /* Spending UINT64_MAX would wrap to 0 and reuse a nonce under the same key. */
    if (s->tx_counter == UINT64_MAX) {
        return OD_SESSION_SEAL_COUNTER_EXHAUSTED;
    }

    build_nonce(s, s->tx_counter, nonce);
    s->tx_counter++;                 /* spent even if the cipher then fails: never reused */

    aad[0] = plain_frame.p[0];
    aad[1] = plain_frame.p[1];
    inner[0] = (uint8_t)payload_len;
    if (payload_len > 0u) {
        memcpy(inner + 1, plain_frame.p + 2, payload_len);
    }

    out[0] = plain_frame.p[0];
    out[1] = plain_frame.p[1];
    memcpy(out + 2, nonce, OD_SESSION_NONCE_LEN);

    cs = od_hal_crypto_ccm_encrypt(s->slot, nonce + 3, OD_SESSION_CCM_NONCE_LEN, aad, 2u,
                                   inner, (uint16_t)(payload_len + 1u),
                                   out + 2 + OD_SESSION_NONCE_LEN,
                                   /* Bounded requirement again -- and here a wrapped capacity
                                    * would burn a tx counter on a spurious failure. */
                                   (uint16_t)(sealed_len - 2u - OD_SESSION_NONCE_LEN), &ct_len);
    if (cs != OD_HAL_CRYPTO_OK) {
        if (report != NULL) {
            report->crypto_status = cs;
        }
        result = OD_SESSION_SEAL_CRYPTO_ERROR;
        goto cleanup;
    }
    *out_len = (uint16_t)(2u + OD_SESSION_NONCE_LEN + ct_len);
    /* OUTBOUND ACTIVITY, and only here: after the cipher succeeded and out_len is final. A
     * preflight refusal produced nothing, and a cipher error may have SPENT a counter but still
     * put no bytes on the wire. Stamping either would keep a link alive on traffic the device
     * declined to answer -- and not stamping this would stop an active client's idle clock and
     * disconnect it mid-transfer. */
    s->last_activity_ms = now_ms;
    result = OD_SESSION_SEAL_OK;

cleanup:
    od_secure_zero(inner, sizeof inner);
    return result;
}

bool od_session_derive_tls_psk(const struct SecurityConfig *sec, uint8_t psk_out[16])
{
    static const char label[] = "opendisplay-tls-psk";

    if (psk_out == NULL || !od_session_security_enabled(sec)) {
        return false;
    }
    return od_hal_crypto_cmac(sec->encryption_key, (const uint8_t *)label,
                              (uint32_t)(sizeof label - 1u), psk_out) == OD_HAL_CRYPTO_OK;
}
