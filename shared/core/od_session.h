/* od_session.h -- BLE session authentication and the AES-CCM envelope, once.
 *
 * WHAT THIS IS. The 0x0050 handshake state machine, the KDF, the session id, the nonce counters
 * and replay window, the timeout/lockout/strike policy, and the envelope framing. ~940 lines on
 * targets/esp32-idf and ~1476 inside targets/nordic-zephyr implemented the same algorithms twice;
 * DIVERGENCE_MATRIX section 6 records that they were byte-identical and maintained by hand. They
 * did not stay that way: the Nordic port silently downgraded the auth-proof compare from
 * constant-time to memcmp, and nothing failed.
 *
 * NO KEY MATERIAL LIVES HERE. The session key goes into an od_hal_crypto slot and this struct
 * keeps only the slot number, so a target that clears a session with memset cannot drop a live
 * vendor key handle -- and a memory dump of this struct yields no key.
 *
 * CONTEXT: single-flow. Every function is called from one context and it is the caller's job to
 * keep it that way -- never an ISR, never a stack callback. No locks, no atomics, no allocation,
 * no blocking. That holds on both targets today (ESP32 touches the session only on the loop task,
 * LAN frames included; Nordic's BT RX thread only enqueues, and the session is reached solely
 * from opendisplay_pipe_process() on main), but it is a precondition rather than an accident:
 * od_watchdog_app.c needed a spinlock the moment its callers spanned contexts.
 *
 * THIS MODULE DOES NOT LOG. od_log.h is target-local and shared/ may not include it, so every
 * fact a target used to print comes back in struct od_session_report and the target says it.
 *
 * NOT PROMOTED, deliberately: device identity (different silicon per target, and it is
 * wire-visible, so it arrives as a parameter); the plaintext-exemption gate and origin plumbing,
 * which belong to od_dispatch; link teardown and the auth-abuse drop, which are connection
 * policy; and secure config erase.
 */
#ifndef OD_SESSION_H
#define OD_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "od_hal_crypto.h"
#include "od_nonce_window.h"
#include "od_span.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- wire sizes, asserted --- */

#define OD_SESSION_NONCE_LEN        16u  /* envelope nonce: session_id(8) || BE64 counter */
#define OD_SESSION_CCM_NONCE_LEN    13u  /* the CCM nonce is envelope nonce[3..15] */
#define OD_SESSION_ID_LEN            8u
#define OD_SESSION_MAC_LEN          16u
#define OD_SESSION_KEY_LEN          16u
#define OD_SESSION_DEVICE_ID_LEN     4u

/* [0x00][0x50][status][server_nonce:16][device_id:4] */
#define OD_SESSION_STEP1_REPLY_LEN  23u
/* [0x00][0x50][status][server_proof:16]. UNDOCUMENTED IN THE SPEC, which says three bytes
 * (opendisplay_protocol.h section 1, CMD_AUTHENTICATE). The 19-byte mutual-auth form is what all
 * four repos ship and what every host expects; DIVERGENCE_MATRIX 6.1 records the gap. Do not
 * "correct" this to match the spec -- correct the spec. */
#define OD_SESSION_STEP2_REPLY_LEN  19u
#define OD_SESSION_REPLY_MAX        OD_SESSION_STEP1_REPLY_LEN
#define OD_SESSION_STEP2_BODY_LEN   32u  /* [client_nonce:16][mac:16], after the opcode bytes */

OD_STATIC_ASSERT(OD_SESSION_NONCE_LEN == ENCRYPTION_NONCE_SIZE, "envelope nonce is 16 bytes");
OD_STATIC_ASSERT(OD_HAL_CRYPTO_TAG_LEN == ENCRYPTION_TAG_SIZE, "CCM tag is 12 bytes");
OD_STATIC_ASSERT(OD_SESSION_CCM_NONCE_LEN == OD_SESSION_NONCE_LEN - 3u,
                 "CCM nonce is envelope nonce[3..15]");
OD_STATIC_ASSERT(OD_SESSION_STEP1_REPLY_LEN ==
                 3u + OD_SESSION_NONCE_LEN + OD_SESSION_DEVICE_ID_LEN, "step-1 reply is 23");
OD_STATIC_ASSERT(OD_SESSION_STEP2_REPLY_LEN == 3u + OD_SESSION_MAC_LEN, "step-2 reply is 19");
OD_STATIC_ASSERT(AUTH_STATUS_SUCCESS == AUTH_STATUS_CHALLENGE,
                 "step-1 and step-2 success share status 0x00; length is the discriminator");

/* ------------------------------------------------------------------------- frame ceilings --- */

/* TWO NUMBERS, AND THEY ANSWER DIFFERENT QUESTIONS.
 *
 * OD_BLE_MAX_FRAME (256) is the ADMISSION bound: the ATT MTU with opcode(1) + handle(2) inside
 * it. RX buffers are sized to it because that is what the GATT layer accepts.
 *
 * 253 is the GENERATION bound: what one write actually carries once ATT takes its three bytes.
 * Nothing this module emits may exceed it. Collapsing the two is how a 253-byte cap silently
 * becomes 256.
 *
 * The envelope is a BLE mechanism and LAN does not widen it. Encrypted LAN is TLS-PSK and bypasses
 * this module entirely -- the TLS handshake IS the authentication, so those frames carry no device
 * nonce (SECTION 9 rule 4). Plaintext LAN still honours the CCM gate in principle, but exactly one
 * port is served at a time and the mDNS tls flag mirrors isEncryptionEnabled(), so plaintext port
 * plus encryption enabled is a defensive state rather than an operating mode. */
#ifndef OD_SESSION_SEALED_MAX
#define OD_SESSION_SEALED_MAX   (OD_BLE_MAX_FRAME - 3u)                  /* 253 */
#endif
#define OD_SESSION_ENVELOPE_MAX (OD_SESSION_SEALED_MAX - 2u)             /* 251, after the cmd */
#define OD_SESSION_PLAIN_MAX    (OD_SESSION_ENVELOPE_MAX - OD_SESSION_NONCE_LEN \
                                 - OD_HAL_CRYPTO_TAG_LEN)                /* 223 = [len:1]+222 */
#define OD_SESSION_PAYLOAD_MAX  (OD_SESSION_PLAIN_MAX - 1u)              /* 222 */
/* The smallest legal envelope: nonce + one encrypted length byte + tag. */
#define OD_SESSION_ENVELOPE_MIN (OD_SESSION_NONCE_LEN + 1u + OD_HAL_CRYPTO_TAG_LEN)  /* 29 */

/* The largest complete [cmd:2][payload] frame od_session_seal accepts. NOT to be confused with
 * OD_SESSION_PLAIN_MAX above: that is the CCM plaintext [len:1][payload] (223), this is what the
 * CALLER hands in (224). They differ by one and mean different things, so seal() bounds against
 * this one in the size_t domain before it narrows anything. */
#define OD_SESSION_PLAIN_FRAME_MAX (2u + OD_SESSION_PAYLOAD_MAX)         /* 224 */

OD_STATIC_ASSERT(OD_SESSION_PAYLOAD_MAX <= 255u, "the inner length prefix is one byte");
OD_STATIC_ASSERT(OD_SESSION_PLAIN_FRAME_MAX + OD_SESSION_NONCE_LEN + 1u + OD_HAL_CRYPTO_TAG_LEN
                 == OD_SESSION_SEALED_MAX, "sealing adds exactly 29 bytes to the plain frame");
OD_STATIC_ASSERT(OD_SESSION_ENVELOPE_MIN < OD_SESSION_ENVELOPE_MAX, "envelope bounds ordered");

/* --------------------------------------------------------------------------- replay window --- */

/* THE REPLAY WINDOW IS NOT DESIGNED HERE. It is ported from the authority repo:
 * ../Firmware/src/nonce_window.h, a zero-dependency state machine with its own 816-line host test
 * (../Firmware/tools/test_nonce_window.cpp). CLAUDE.md: Firmware is the authority, and working
 * implementations are imported as-is rather than re-derived. C4 lands it as
 * shared/core/od_nonce_window.h with the od_ prefix and no logic change.
 *
 * Do not substitute the ±32 signed-difference form still present in this repo's imported
 * targets/esp32-idf/src/encryption.cpp. Upstream replaced it, and the two reasons are both
 * security-relevant:
 *
 *   - THE FORWARD DIRECTION IS UNBOUNDED. Any counter above last_seen is accepted; only the
 *     BACKWARD window is bounded. A forward cap cannot be sized safely -- once a gap exceeded it
 *     nothing would commit, last_seen would never advance, and every later frame would be
 *     rejected further out than the last until re-authentication. That is a stranded session, not
 *     a rejected frame.
 *   - COMPARISON IS PLAIN NUMERIC, NEVER MODULAR OR SIGNED. The counter is parsed off the wire
 *     BEFORE the tag is verified, so an attacker controls both operands. Signed differences mean
 *     a uint64_t >= 2^63 converted to int64_t (implementation-defined before C++20), signed
 *     overflow, and negating INT64_MIN -- three ways to be undefined on attacker-chosen input.
 *     Only unsigned comparison and one subtraction guarded by its own branch are used.
 *
 * NonceResult is FOUR-valued upstream: NONCE_OK, NONCE_REPLAY, NONCE_OUT_OF_WINDOW and
 * NONCE_BAD_SESSION -- the session-id mismatch is a nonce result there, not a separate branch.
 * The caller DOES act differently on them, so the reason reaches it: a nonce-reason rejection is
 * ordinary packet loss on a lossy link and must not be answered with a fatal NACK on the pipe
 * path, while a tag failure is tamper evidence. report->nonce_reason carries which.
 *
 * ORDERING: od_nonce_check() decides, CCM verifies, and only then does od_nonce_commit() run.
 * That is RFC 4303 Appendix A2's "if the MAC is valid, the window is updated", and it is what
 * stops a forged frame from moving last_seen. */
/* OD_NONCE_BACKWARD_BITS and OD_NONCE_BITMAP_WORDS come from od_nonce_window.h, which is the
 * ported upstream header -- they are deliberately NOT redefined here. */

/* ------------------------------------------------------------------------------- policy --- */

#define OD_SESSION_CHALLENGE_WINDOW_MS 30000u
#define OD_SESSION_RATE_WINDOW_MS      60000u
#define OD_SESSION_RATE_MAX_ATTEMPTS      10u
#define OD_SESSION_INTEGRITY_STRIKES       3u

/* ------------------------------------------------------------------------------- results --- */

enum od_session_auth {
    OD_SESSION_AUTH_CHALLENGE = 0,   /* step 1 accepted; reply is 23 bytes, no session yet */
    OD_SESSION_AUTH_ESTABLISHED,     /* step 2 verified; reply is 19 bytes, session OPEN */
    OD_SESSION_AUTH_REJECTED,        /* wrong key: AUTH_STATUS_FAILED */
    OD_SESSION_AUTH_RATE_LIMITED,    /* AUTH_STATUS_RATE_LIMIT */
    OD_SESSION_AUTH_NOT_CONFIGURED,  /* AUTH_STATUS_NOT_CONFIG; also what a NULL sec means */
    OD_SESSION_AUTH_MALFORMED,       /* AUTH_STATUS_ERROR: bad length or no challenge pending */
    OD_SESSION_AUTH_EXPIRED,         /* AUTH_STATUS_ERROR: challenge older than 30 s */
    OD_SESSION_AUTH_CRYPTO_ERROR,    /* AUTH_STATUS_ERROR: the engine failed, not the client */
    OD_SESSION_AUTH_NO_ROOM,         /* rsp_cap too small for this step's reply */
    OD_SESSION_AUTH_BAD_ARGUMENT     /* s/rsp/rsp_len/device_id NULL: no reply is possible */
};

enum od_session_open {
    OD_SESSION_OPEN_OK = 0,
    OD_SESSION_OPEN_NO_SESSION,      /* not authenticated, or expired on this call */
    OD_SESSION_OPEN_SHORT,           /* envelope below OD_SESSION_ENVELOPE_MIN, or bad argument */
    OD_SESSION_OPEN_TOO_LONG,        /* envelope above OD_SESSION_ENVELOPE_MAX */
    OD_SESSION_OPEN_WRONG_SESSION,   /* nonce session_id mismatch */
    OD_SESSION_OPEN_REPLAY,          /* NONCE_REPLAY or NONCE_OUT_OF_WINDOW; report says which */
    OD_SESSION_OPEN_BAD_TAG,         /* CCM authentication failed */
    OD_SESSION_OPEN_BAD_LENGTH,      /* inner length byte disagrees with the decrypted size */
    OD_SESSION_OPEN_NO_ROOM,         /* out_cap too small */
    OD_SESSION_OPEN_CRYPTO_ERROR     /* engine fault; NOT counted as a strike */
};

enum od_session_seal {
    OD_SESSION_SEAL_OK = 0,
    OD_SESSION_SEAL_NO_SESSION,
    OD_SESSION_SEAL_TOO_SHORT,       /* plain_frame below 2: no cmd bytes to echo or use as AAD */
    OD_SESSION_SEAL_TOO_LONG,        /* payload above OD_SESSION_PAYLOAD_MAX */
    OD_SESSION_SEAL_NO_ROOM,         /* out_cap too small; checked before a nonce is drawn */
    OD_SESSION_SEAL_CRYPTO_ERROR,
    OD_SESSION_SEAL_COUNTER_EXHAUSTED /* tx_counter reached UINT64_MAX; re-auth required */
};

/* Everything the three targets used to log from inside these functions. Every field is filled
 * on a best-effort basis and report may always be NULL. */
struct od_session_report {
    uint8_t  status_byte;         /* the AUTH_STATUS_* actually placed in the reply */
    uint8_t  attempts;            /* auth attempts inside the current rate window */
    uint8_t  integrity_failures;  /* after this call -- EXCEPT on teardown, where it is the
                                   * count that triggered it (3); the session's own field is
                                   * zeroed by the clear that follows. The reported value is
                                   * the useful one. */
    bool     torn_down;           /* this call cleared the session */
    uint64_t rx_counter;          /* counter parsed from the inbound nonce */
    uint64_t rx_last;             /* rx_last BEFORE this call */
    int32_t  rx_diff;             /* saturated counter - rx_last, for the out-of-window log */
    uint32_t age_ms;              /* session age at the timeout check */
    enum od_hal_crypto_status crypto_status;  /* why CRYPTO_ERROR, when one occurred */
    /* Which nonce rule rejected the frame, so the caller can tell ordinary packet loss from
     * tamper evidence. Mirrors upstream's NonceResult; 0 means "not a nonce rejection". */
    uint8_t  nonce_reason;
};

/* ----------------------------------------------------------------------------- the state --- */

struct od_session {
    bool     authenticated;
    uint8_t  session_id[OD_SESSION_ID_LEN];
    od_hal_crypto_slot_t slot;    /* CONFIGURATION, not session state: survives a clear */
    bool     key_loaded;          /* the slot currently holds this session's key */

    /* device->host, monotonic. MUST be zeroed on every re-authentication, not carried across:
     * a device that kept its outbound counter while the client restarts at 0 reproduces keystream
     * reuse against itself under the new session key. Upstream resets it with the other three
     * nonce fields in one place for exactly this reason. */
    uint64_t tx_counter;
    uint64_t rx_last;             /* highest ACCEPTED inbound counter */
    uint64_t rx_seen[OD_NONCE_BITMAP_WORDS];  /* bit i == counter (rx_last - i) consumed; bit 0 is
                                               * rx_last. 256 bits of reordering tolerance; the
                                               * width is NOT a replay-security parameter and is
                                               * deliberately not derived from the PIPE window. */

    uint32_t session_start_ms;    /* ABSOLUTE lifetime basis, never last-activity */
    uint32_t last_activity_ms;    /* diagnostics and the target's idle policy; NOT the timeout */
    uint32_t timeout_ms;          /* session_timeout_seconds * 1000, snapshotted at auth */
    uint32_t challenge_ms;
    bool     challenge_pending;   /* not challenge_ms == 0: a challenge issued at uptime 0 is real */
    uint32_t last_auth_ms;
    uint8_t  auth_attempts;       /* 0 also means "no rate window open" */
    uint8_t  integrity_failures;

    uint8_t  pending_server_nonce[OD_SESSION_NONCE_LEN];
};

/* A RAM ratchet, not a coincidence: this struct lands on a part with 32 KB total. */
/* A RAM ratchet. 32 B of this is the ported 256-bit replay bitmap, which still returns 480 B
 * against the 512 B ring it replaces. */
OD_STATIC_ASSERT(sizeof(struct od_session) <= 128u, "od_session state grew past its BG22 budget");

/* ---------------------------------------------------------------------------- interface --- */

/* Bind the HAL slot. Call ONCE, on zero-initialised storage. Does not touch the slot contents. */
void od_session_init(struct od_session *s, od_hal_crypto_slot_t slot);

/* THE ONLY TEARDOWN. Releases the HAL key slot, then zeroes -- but PRESERVES s->slot, which is
 * configuration. Zeroing it would move every session after the first timeout onto slot 0.
 * A target MUST NOT memset a struct od_session itself: that drops the slot without releasing it.
 * Idempotent and NULL-safe. */
void od_session_clear(struct od_session *s);

/* Is encryption in force? The zero-key rule, applied once for every target, via
 * od_config_security_key_set(). A NULL sec is "no". */
bool od_session_security_enabled(const struct SecurityConfig *sec);

/* Has a session been established and not cleared. Pure: does not consider time. */
bool od_session_authenticated(const struct od_session *s);

/* Live now?
 *
 * MUTATING BY DESIGN: an expired session is CLEARED here, which every shipped call site already
 * relies on. Timeout is ABSOLUTE from session_start_ms, computed as an unsigned ms-domain
 * subtraction so it is correct across the 32-bit rollover -- unlike the shipped ESP32 form, which
 * divides both timestamps by 1000 before subtracting and therefore tears the session down once
 * per 49.7 days of uptime. timeout_ms == 0 means no expiry.
 *
 * BOUNDARY: expires on elapsed >= timeout_ms. Note this is the OPPOSITE convention from the
 * challenge window below, which uses >. Both match what shipped; do not harmonise them. */
bool od_session_alive(struct od_session *s, uint32_t now_ms, struct od_session_report *report);

/* Stamp activity without decrypting -- for the target's idle-link policy only. NOT the timeout
 * basis. This is the "activity stamp" the dispatch plan scopes by origin. */
void od_session_touch(struct od_session *s, uint32_t now_ms);

/* The whole 0x0050 machine, both steps.
 *
 * body is the frame AFTER the two opcode bytes: one 0x00 byte for step 1, or
 * OD_SESSION_STEP2_BODY_LEN bytes of [client_nonce:16][mac:16] for step 2. device_id is a
 * PARAMETER because the two targets derive it from different silicon and it feeds both the KDF
 * and the proof -- a HAL call would hide a wire-visible identity behind an interface.
 *
 * RETURNS THE REPLY, never sends it: a handler that sends behind the dispatcher's back cannot be
 * routed by origin or counted for auth abuse. rsp needs OD_SESSION_REPLY_MAX bytes.
 *
 * TRANSACTIONAL. Capacity is checked BEFORE any state changes, so a short buffer cannot leave a
 * challenge minted or a session opened that the client can never learn about.
 *
 * CHALLENGE LIFECYCLE. challenge_pending is set only after the RNG succeeds. Step 2 requires it,
 * and consumes it on every outcome that reaches the proof check -- success, wrong proof, expiry,
 * malformed body, crypto failure -- so one challenge answers one step 2 and an attacker cannot
 * grind proofs against a fixed server nonce. RATE_LIMITED, BAD_ARGUMENT and NO_ROOM do NOT consume
 * it, and none of the three mutates any session state. The stated reason for RATE_LIMITED
 * preserving it -- "so a throttled client can answer the challenge it already holds" -- is not
 * actually reachable: the challenge window is 30 s and the rate window 60 s, so any preserved
 * challenge has expired before the peer is unthrottled. Preserving it is still right (a refusal
 * that never examined the request should not consume state), but do not defend it with that
 * scenario. NO_ROOM does READ the body far enough to know which
 * reply size is required -- that is what makes the capacity preflight step-specific, and it must
 * stay ahead of the rate limiter, which is the first thing that mutates.
 *
 * A step 1 arriving over a live session CLEARS it and issues a fresh challenge, which is what
 * shipped; AUTH_STATUS_ALREADY is defined by the protocol and never sent.
 *
 * Expiry boundary is elapsed > OD_SESSION_CHALLENGE_WINDOW_MS, so exactly 30000 ms is ACCEPTED. */
enum od_session_auth od_session_authenticate(struct od_session *s,
        const struct SecurityConfig *sec,
        const uint8_t device_id[OD_SESSION_DEVICE_ID_LEN],
        od_span_t body, uint32_t now_ms,
        uint8_t *rsp, size_t rsp_cap, uint16_t *rsp_len,
        struct od_session_report *report);

/* Decrypt one inbound envelope: [nonce:16][ciphertext][tag:12], the frame after the opcode bytes.
 * cmd is the 16-bit opcode; its two big-endian bytes are the AAD.
 *
 * ORDERING IS THE SECURITY PROPERTY: the replay window is CHECKED before the tag and ADVANCED only
 * after it verifies, so a forged frame cannot move rx_last and lock out legitimate lower-counter
 * frames. This port is NOT the first to get that right -- upstream Firmware already made
 * nonceCommit() the first statement of the success arm (encryption.cpp:790). The targets that
 * advanced before decrypting were the two Nordic/Silabs ports.
 *
 * out_cap must cover the DECRYPTED FRAME (>= ct_len - tag), one byte more than *out_len will be:
 * CCM emits [len:1][payload] and the length byte is plaintext the cipher writes. The payload is
 * moved down over it before returning. out never aliases the input.
 *
 * ON ANY NON-OK RETURN out IS UNDEFINED AND MUST NOT BE READ -- CCM is decrypt-then-verify.
 *
 * STRIKES: ONLY BAD_TAG counts toward the 3-strike teardown. Corrected against upstream
 * Firmware, which made this change deliberately: nonce failures are evidence of a LOSSY LINK, not
 * of tampering, so REPLAY, OUT_OF_WINDOW and WRONG_SESSION must NOT count -- a session-id mismatch
 * is exactly what a stale client sends after the device re-authenticated. Counting them lets
 * ordinary packet loss tear down a live session, which is the same self-DoS shape as answering a
 * retransmission with a strike. Only the CCM tag is a tamper oracle. CRYPTO_ERROR does not count
 * either: an engine fault is not an attack.
 *
 * A target that logs these must rate-limit per site (upstream uses a 5 s budget for each), because
 * once they stop counting toward the strike limit nothing else throttles a peer driving them, and
 * out-of-window fires routinely on a lossy link. */
enum od_session_open od_session_open(struct od_session *s, uint16_t cmd, od_span_t envelope,
        uint8_t *out, size_t out_cap, uint16_t *out_len,
        uint32_t now_ms, struct od_session_report *report);

/* Seal one outbound frame.
 *
 * plain_frame is [cmd:2][payload], NOT payload alone: the two leading bytes are both echoed into
 * the output and used as the AAD, which is the shape the shipped encoder already takes. Fewer than
 * two bytes has no cmd to echo and returns TOO_SHORT.
 *
 * Output is [cmd:2][nonce:16][len:1][payload][tag:12]; out needs plain_frame.n + 29 bytes, and
 * capacity is checked BEFORE a nonce is drawn or tx_counter advances, so a short buffer never
 * burns a counter value.
 *
 * The counter advances on every call that reaches the cipher, including one that then fails, so
 * the TX counter never repeats -- except at the top, where spending UINT64_MAX would wrap to 0 and
 * reuse a nonce under the same key. That returns COUNTER_EXHAUSTED and requires re-authentication.
 *
 * THIS IS TX-SIDE NON-REUSE ONLY. Inbound and outbound share one session_id and both counters
 * start at 0, so the same session_id||counter nonce is used in both directions under one key. That
 * is a protocol-level flaw this module cannot fix; see FOLLOWUPS.md. Do not let this comment grow
 * into a claim that nonces are globally unique. */
enum od_session_seal od_session_seal(struct od_session *s, od_span_t plain_frame,
        uint8_t *out, size_t out_cap, uint16_t *out_len,
        uint32_t now_ms, struct od_session_report *report);

/* The LAN TLS-PSK pre-shared key: AES-CMAC over a fixed label under the master key. Same KDF
 * primitive, no nonces. Returns false when encryption is not configured. */
bool od_session_derive_tls_psk(const struct SecurityConfig *sec, uint8_t psk_out[16]);

#ifdef __cplusplus
}
#endif

#endif /* OD_SESSION_H */
