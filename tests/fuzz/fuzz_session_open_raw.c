/* fuzz_session_open_raw.c -- the input IS the envelope.
 *
 * Covers everything upstream of the cipher: the length gate at OD_SESSION_ENVELOPE_MIN/MAX, the
 * session-id compare, the replay window, and the span arithmetic that turns an under-length frame
 * into a huge unsigned length if the bounds check is ever moved after the subtraction.
 *
 * It reaches the POST-decrypt path essentially never, because random bytes cannot forge a CCM
 * tag. That is not a defect of this target -- it is why fuzz_session_open_sealed.c exists.
 *
 * THE ADDRESSED SESSION-ID BYTE. A first cut fed the input through verbatim and plateaued at 24
 * edges: the session id is eight bytes the fuzzer has to hit exactly, so every input stopped at
 * WRONG_SESSION and the replay window -- the point of this target -- was never reached. A leading
 * control byte now decides whether the real session id is spliced in, keeping BOTH the mismatch
 * branch and everything behind it reachable.
 */

#include "fuzz_session.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct od_session s;
    uint8_t envelope[OD_SESSION_ENVELOPE_MAX + 16u];
    uint8_t *out;
    uint16_t out_len = 0xFFFFu;
    uint64_t rx_last_before;
    uint8_t  strikes_before;
    uint64_t counter = UINT64_MAX;
    enum od_session_open r;
    size_t cap;
    size_t n;
    uint8_t ctl;
    unsigned k;

    if (size < 1u) { return 0; }
    ctl  = data[0];
    data += 1;
    size -= 1u;

    /* A little past OD_SESSION_ENVELOPE_MAX so the over-long rejection is reachable, but bounded
     * -- the wire cannot deliver more, and unbounded lengths only dilute the corpus. */
    n = (size > sizeof envelope) ? sizeof envelope : size;
    if (n > 0u) { memcpy(envelope, data, n); }

    fz_open_session(&s);

    /* Bit 0: address the frame to this session. Without it the 8-byte compare is a 2^-64 lottery
     * the fuzzer never wins, and everything past it is dead code to the mutator. */
    if ((ctl & 1u) != 0u && n >= OD_SESSION_ID_LEN) {
        memcpy(envelope, s.session_id, OD_SESSION_ID_LEN);
    }

    /* The counter the envelope claims, for the accept-case invariant. Below the minimum envelope
     * there are no such bytes and no accept is possible. */
    if (n >= OD_SESSION_NONCE_LEN) {
        counter = 0u;
        for (k = 0u; k < 8u; ++k) { counter = (counter << 8) | envelope[8u + k]; }
    }

    rx_last_before = s.rx_last;
    strikes_before = s.integrity_failures;

    /* Exactly the documented capacity, on the heap: an overrun lands in an ASan red zone rather
     * than in slack. */
    cap = OD_SESSION_PLAIN_MAX;
    out = (uint8_t *)malloc(cap);
    FZ_ASSERT(out != NULL);

    r = od_session_open(&s, 0x0071u, od_span_make(envelope, n), out, cap, &out_len, 2000u, NULL);

    fz_check_open(r, &s, rx_last_before, strikes_before, counter, out_len);
    free(out);
    return 0;
}
