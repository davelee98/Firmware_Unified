/* session_fake.h -- the fake crypto HAL and the shared fixture, for every od_session host binary.
 *
 * The fake is bound at LINK time, exactly as a target's HAL is: shared/ has no injection seam by
 * design, so linking is what gets exercised. Consumers link session_fake.c and od_shared.
 *
 * The soft-CCM oracle behind the fake is the RFC 3610 implementation targets/nordic-zephyr
 * shipped (session_ccm_reference.inc), kept verbatim so the tags these tests compute come from
 * the shipped algorithm rather than from a re-derivation.
 */

#ifndef OD_TEST_SESSION_FAKE_H
#define OD_TEST_SESSION_FAKE_H

#include "od_session.h"

#include <stdbool.h>
#include <stdint.h>

/* AES-CMAC (RFC 4493) on the host. The differential reference for the KDF and the auth proof --
 * validated against all four RFC 4493 vectors by the aes128 suite. */
void host_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t out[16]);

/* ------------------------------------------------------------------------- the fake's state --- */

/* Call before every case: clears the slots, rewinds the deterministic RNG, and stops any injected
 * error. A case that forgets this inherits the previous one's slot contents. */
void fake_reset(void);

extern unsigned g_key_set_calls;    /* slot hygiene: no leak across repeated re-authentication */
extern unsigned g_key_clear_calls;
extern uint8_t  g_slot_key[OD_HAL_CRYPTO_KEY_SLOTS][16];

/* Set to a non-OK status to make every HAL entry point fail with it. This is how the tests reach
 * the engine-fault path, which must NOT count an integrity strike -- the distinction the
 * four-valued status exists for. */
extern enum od_hal_crypto_status g_force_status;

/* How many HAL calls to let succeed before g_force_status starts being returned. 0 (the
 * fake_reset default) fails from the first call; N reaches the N+1'th; -1 injects nothing.
 * Without this a single flag is swallowed by whichever call happens to come first, which is why
 * most of od_session.c's crypto-failure exits had no coverage. g_hal_calls is the running count
 * and is readable so a test can assert how far it got. */
extern int32_t  g_force_after;
extern unsigned g_hal_calls;

/* Make the Nth CMAC succeed but return an all-zero MAC. The only way to reach od_session.c's
 * all-zero-session-id rejection, which cannot be produced by an error injection because the
 * derivation has to SUCCEED and yield zeros. -1 = never. */
extern int32_t  g_zero_cmac_after;

/* ----------------------------------------------------------------------------- the fixture --- */

extern const uint8_t MASTER[16];
extern const uint8_t DEVICE_ID[4];
extern struct SecurityConfig g_sec;

/* timeout_s == 0 means no expiry, as on the wire. */
void sec_init(uint16_t timeout_s);

/* Drive a full handshake. Returns the od_session_auth of step 2 and, on success, leaves the
 * session open. server_nonce_out receives the challenge so callers can recompute proofs. */
enum od_session_auth handshake(struct od_session *s, uint32_t now_ms,
                               uint8_t server_nonce_out[16], bool corrupt_proof);

/* The client nonce handshake() uses. Exposed so a caller can recompute the device's own
 * mutual-auth proof and check the 16 bytes it returns -- nothing did, which left the one
 * wire-visible derivation in the handshake with no coverage at all. */
extern const uint8_t CLIENT_NONCE[16];

/* As handshake(), but the step-2 reply is handed back rather than discarded. */
enum od_session_auth handshake_capture(struct od_session *s, uint32_t now_ms,
                                       uint8_t server_nonce_out[16],
                                       uint8_t *rsp, uint16_t *rsp_len);

/* Step 1 ONLY: mint a challenge and stop, for cases about what happens to a PENDING challenge.
 * Returns true when the challenge was issued; server_nonce_out and rsp receive the reply. */
bool handshake_step1(struct od_session *s, uint32_t now_ms, uint8_t server_nonce_out[16],
                     uint8_t *rsp, uint16_t *rsp_len);

#endif /* OD_TEST_SESSION_FAKE_H */
