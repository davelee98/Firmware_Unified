/* fuzz_session.h -- shared scaffolding for the od_session libFuzzer targets.
 *
 * TWO RULES THIS FILE EXISTS TO ENFORCE, both of them ways a fuzz target silently becomes a
 * no-op while reporting healthy coverage:
 *
 * 1. EVERY INPUT STARTS FROM A FRESH, OPEN SESSION. Carrying one session across inputs looks
 *    cheaper and is wrong twice: rx_last ratchets up so later inputs are rejected by the window
 *    before reaching the parser, and three bad tags tear the session down, after which every
 *    remaining input returns NO_SESSION. It also destroys reproducibility -- a crash would depend
 *    on the whole preceding input sequence rather than on the file libFuzzer writes out.
 *
 * 2. OUT BUFFERS ARE malloc'd AT EXACTLY THE ADVERTISED CAPACITY. A stack array with slack hides
 *    a one-byte overrun; an exact-size heap block puts an ASan red zone on the very next byte.
 *    od_session_open's out_cap arithmetic is where such a bug was already found once by review.
 */

#ifndef OD_FUZZ_SESSION_H
#define OD_FUZZ_SESSION_H

#include "od_session.h"
#include "session_fake.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Not assert(): NDEBUG is set by some release-flavoured configurations and would silently delete
 * every check in this file, leaving targets that can only ever find an ASan hit. */
#define FZ_ASSERT(cond)                                                    \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "INVARIANT %s:%d: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                \
            abort();                                                       \
        }                                                                  \
    } while (0)

#include <stdio.h>

/* A byte reader over the fuzzer's input. Runs dry rather than over-reading: once exhausted it
 * yields zeros, so a short input is a valid (if boring) case instead of a bounds bug in the
 * harness itself. */
struct fz_cursor {
    const uint8_t *p;
    size_t         n;
    size_t         i;
};

static inline void fz_cursor_init(struct fz_cursor *c, const uint8_t *p, size_t n)
{
    c->p = p; c->n = n; c->i = 0u;
}

static inline uint8_t fz_u8(struct fz_cursor *c)
{
    return (c->i < c->n) ? c->p[c->i++] : 0u;
}

static inline uint64_t fz_u64(struct fz_cursor *c)
{
    uint64_t v = 0u;
    unsigned k;
    for (k = 0u; k < 8u; ++k) { v = (v << 8) | fz_u8(c); }
    return v;
}

/* Whatever is left, as a span into the input. */
static inline const uint8_t *fz_rest(struct fz_cursor *c, size_t *n_out)
{
    *n_out = (c->i < c->n) ? (c->n - c->i) : 0u;
    return c->p + ((c->i < c->n) ? c->i : c->n);
}

/* --------------------------------------------------------------------------- fresh session --- */

/* Reset the fake and drive a real handshake. Aborts on failure: an unopenable session means the
 * harness is broken, and continuing would fuzz nothing but the NO_SESSION branch. */
static inline void fz_open_session(struct od_session *s)
{
    uint8_t server_nonce[16];

    fake_reset();
    sec_init(0u);                       /* no expiry: the clock is an input, not a nuisance */
    memset(s, 0, sizeof *s);
    od_session_init(s, 0u);
    FZ_ASSERT(handshake(s, 1000u, server_nonce, false) == OD_SESSION_AUTH_ESTABLISHED);
    FZ_ASSERT(od_session_authenticated(s));
}

/* ------------------------------------------------------------------------------ invariants --- */

/* Everything od_session_open must satisfy for ANY input, checked against state captured before
 * the call. `counter` is the counter the envelope carried, or UINT64_MAX if the input was too
 * short to carry one (in which case the accept case cannot arise). */
static inline void fz_check_open(enum od_session_open r,
                                 const struct od_session *s,
                                 uint64_t rx_last_before,
                                 uint8_t strikes_before,
                                 uint64_t counter,
                                 uint16_t out_len)
{
    FZ_ASSERT(r <= OD_SESSION_OPEN_CRYPTO_ERROR);

    /* One call can spend at most one strike, and these harnesses open a fresh session per input,
     * so the 3-strike teardown is unreachable here. Asserted rather than assumed -- if it ever
     * fires, the state below has been cleared and every other invariant is meaningless. */
    FZ_ASSERT(od_session_authenticated(s));

    if (r == OD_SESSION_OPEN_OK || r == OD_SESSION_OPEN_BAD_LENGTH) {
        /* THE WINDOW ADVANCES EXACTLY WHEN THE TAG VERIFIED -- which includes BAD_LENGTH, because
         * that frame was authentic and leaving it uncommitted would leave it replayable. Forward
         * moves the high-water mark to the counter; a backfill leaves it alone. */
        FZ_ASSERT(s->rx_last == ((counter > rx_last_before) ? counter : rx_last_before));
    } else {
        /* THE ORDERING PROPERTY. A frame whose tag did NOT verify -- including a forgery at a
         * counter far ahead -- must not move rx_last, or an attacker locks out the legitimate
         * lower-counter frames still in flight. This is the shipped Nordic bug. */
        FZ_ASSERT(s->rx_last == rx_last_before);
    }

    if (r == OD_SESSION_OPEN_OK) {
        FZ_ASSERT(out_len <= OD_SESSION_PAYLOAD_MAX);
        FZ_ASSERT(s->integrity_failures == 0u);   /* success resets the strike count */
    } else {
        FZ_ASSERT(out_len == 0u);                 /* nothing is reported on a failure */
    }

    /* Strikes are the teardown budget, and only the CCM tag may spend one. Counting a replay or a
     * stale session-id would let ordinary packet loss tear a live session down; counting an
     * engine fault would turn a transient OOM into forced re-authentication. */
    if (r == OD_SESSION_OPEN_BAD_TAG) {
        FZ_ASSERT(s->integrity_failures == (uint8_t)(strikes_before + 1u));
    } else {
        FZ_ASSERT(s->integrity_failures ==
                  ((r == OD_SESSION_OPEN_OK) ? 0u : strikes_before));
    }
}

#endif /* OD_FUZZ_SESSION_H */
