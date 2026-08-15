/* fuzz_session_open_sealed.c -- every input decrypts, so the fuzzer explores the far side.
 *
 * WHY THIS TARGET EXISTS. A fuzzer cannot forge a CCM tag, so in fuzz_session_open_raw.c
 * essentially every input dies at the tag check and the code AFTER decrypt is never reached --
 * while coverage counters look healthy. That code is where the interesting arithmetic lives: the
 * exact inner-length check, the memmove of the payload down over the length byte, and the
 * out_cap bound (already wrong by one byte once, caught in review rather than by a test).
 *
 * So this harness does not take the envelope from the fuzzer. It takes the FIELDS -- counter,
 * declared inner length, payload, and the caller's out_cap -- and seals them with a genuine tag
 * through the same fake HAL the session will use to open them. The declared length is
 * deliberately independent of the real payload length; that mismatch is the bug class.
 */

#include "fuzz_session.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct od_session s;
    struct fz_cursor cur;
    uint8_t  envelope[OD_SESSION_NONCE_LEN + OD_SESSION_PLAIN_MAX + OD_HAL_CRYPTO_TAG_LEN];
    uint8_t  inner[OD_SESSION_PLAIN_MAX];
    uint8_t  ccm_aad[2];
    uint8_t *out;
    uint16_t out_len = 0xFFFFu;
    uint16_t ct_len = 0u;
    uint16_t cmd;
    uint64_t counter;
    uint64_t rx_last_before;
    uint8_t  strikes_before;
    uint8_t  declared;
    size_t   payload_n;
    size_t   out_cap;
    size_t   inner_len;
    const uint8_t *payload;
    enum od_session_open r;
    enum od_hal_crypto_status cs;

    fz_open_session(&s);
    fz_cursor_init(&cur, data, size);

    counter  = fz_u64(&cur);
    declared = fz_u8(&cur);          /* the inner length byte -- NOT tied to the real payload */
    cmd      = (uint16_t)(((uint16_t)fz_u8(&cur) << 8) | fz_u8(&cur));
    /* The caller's capacity, swept across the whole legal range and one past it. od_session_open
     * must never write beyond it, whatever the sealed frame claims. */
    out_cap  = (size_t)fz_u8(&cur) % (OD_SESSION_PLAIN_MAX + 1u);

    payload = fz_rest(&cur, &payload_n);
    if (payload_n > OD_SESSION_PAYLOAD_MAX) { payload_n = OD_SESSION_PAYLOAD_MAX; }

    /* [len:1][payload] -- the plaintext CCM actually carries. */
    inner[0] = declared;
    if (payload_n > 0u) { memcpy(inner + 1, payload, payload_n); }
    inner_len = payload_n + 1u;

    /* [session_id:8][BE64 counter], the envelope nonce od_session_open will parse back out. */
    memcpy(envelope, s.session_id, OD_SESSION_ID_LEN);
    {
        unsigned k;
        for (k = 0u; k < 8u; ++k) {
            envelope[OD_SESSION_ID_LEN + k] = (uint8_t)(counter >> (56u - 8u * k));
        }
    }

    /* Seal with the fake HAL directly, mirroring od_session_open's own derivation: the CCM nonce
     * is envelope nonce[3..15] and the AAD is the two opcode bytes. If those ever diverge this
     * target degrades to fuzz_session_open_raw -- which is why the OK path is asserted below. */
    ccm_aad[0] = (uint8_t)((cmd >> 8) & 0xFFu);
    ccm_aad[1] = (uint8_t)(cmd & 0xFFu);
    cs = od_hal_crypto_ccm_encrypt(0u, envelope + 3, OD_SESSION_CCM_NONCE_LEN, ccm_aad, 2u,
                                   inner, (uint16_t)inner_len,
                                   envelope + OD_SESSION_NONCE_LEN,
                                   (uint16_t)(sizeof envelope - OD_SESSION_NONCE_LEN), &ct_len);
    FZ_ASSERT(cs == OD_HAL_CRYPTO_OK);

    rx_last_before = s.rx_last;
    strikes_before = s.integrity_failures;

    /* malloc'd at exactly the advertised capacity: a one-byte overrun hits an ASan red zone
     * instead of stack slack. A zero capacity is legal and must still be refused cleanly. */
    out = (uint8_t *)malloc(out_cap != 0u ? out_cap : 1u);
    FZ_ASSERT(out != NULL);

    r = od_session_open(&s, cmd,
                        od_span_make(envelope, OD_SESSION_NONCE_LEN + (size_t)ct_len),
                        out, out_cap, &out_len, 2000u, NULL);

    fz_check_open(r, &s, rx_last_before, strikes_before, counter, out_len);

    /* The tag is genuine by construction, so a tag failure means the harness and od_session_open
     * disagree about the nonce or the AAD -- the silent way this target stops testing anything. */
    FZ_ASSERT(r != OD_SESSION_OPEN_BAD_TAG);

    /* The declared length is the whole point: it is honoured exactly, or the frame is refused. */
    if (r == OD_SESSION_OPEN_OK) {
        FZ_ASSERT((size_t)declared == payload_n);
        FZ_ASSERT(out_len == declared);
        FZ_ASSERT(memcmp(out, payload, out_len) == 0);
    } else if ((size_t)declared != payload_n && out_cap >= inner_len) {
        /* Enough room to decrypt, and a mismatched length: BAD_LENGTH is the only honest answer.
         * Anything else means the check was skipped or the frame was rejected for the wrong
         * reason. */
        FZ_ASSERT(r == OD_SESSION_OPEN_BAD_LENGTH);
    }

    free(out);
    return 0;
}
