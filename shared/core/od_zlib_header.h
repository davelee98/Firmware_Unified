/* od_zlib_header — the zlib stream header rule, in one place for every inflate engine.
 *
 * RFC 1950 fixes the two leading bytes, and this fleet narrows them further: the declared window
 * must fit OPENDISPLAY_ZLIB_WINDOW_BITS, which is a wire contract with the host encoder rather
 * than a buffer size. A target whose engine applies a different limit accepts streams the rest of
 * the fleet refuses, and the difference is invisible until a device meets a stream the others
 * reject — so the rule lives here and every engine calls it.
 *
 * Two entry points, one rule:
 *   od_zlib_header_check()   — both bytes in hand (a decoder that parses the header itself).
 *   od_zlib_header_observe() — bytes arriving across pushes (an engine that parses its own header
 *                              and must be refused BEFORE it sees an over-wide stream). Observing
 *                              does not consume: the caller still forwards the same bytes on.
 *
 * All static inline: no tier, no source entry, no cost to a target that does not call it.
 */

#ifndef OD_ZLIB_HEADER_H
#define OD_ZLIB_HEADER_H

#include "od_zlib_inflate.h"   /* OPENDISPLAY_ZLIB_WINDOW_BITS */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OD_ZLIB_HEADER_OK = 0,
    OD_ZLIB_HEADER_NEED_MORE,      /* fewer than two bytes seen so far */
    OD_ZLIB_HEADER_BAD,            /* not a zlib header: check bits, method, or preset dictionary */
    OD_ZLIB_HEADER_WINDOW_TOO_BIG  /* well-formed, but declares more history than this build keeps */
} od_zlib_header_result_t;

static inline od_zlib_header_result_t od_zlib_header_check(uint8_t cmf, uint8_t flg)
{
    /* CM must be 8 (deflate), FCHECK must make the pair a multiple of 31, and FDICT must be
     * clear: a preset dictionary is a stream this fleet has no way to supply. */
    if (((256u * (unsigned)cmf + (unsigned)flg) % 31u) != 0u ||
        (cmf & 0x0Fu) != 8u ||
        (flg & 0x20u) != 0u) {
        return OD_ZLIB_HEADER_BAD;
    }
    if (((unsigned)(cmf >> 4) + 8u) > OPENDISPLAY_ZLIB_WINDOW_BITS) {
        return OD_ZLIB_HEADER_WINDOW_TOO_BIG;
    }
    return OD_ZLIB_HEADER_OK;
}

/* Streaming form. `have` counts header bytes seen; once both are validated the result is OK for
 * the rest of the stream, so a caller may run this on every push unconditionally. */
typedef struct {
    uint8_t cmf;
    uint8_t have;
    bool    validated;
} od_zlib_header_t;

static inline void od_zlib_header_reset(od_zlib_header_t *h)
{
    h->cmf = 0u;
    h->have = 0u;
    h->validated = false;
}

static inline od_zlib_header_result_t od_zlib_header_observe(od_zlib_header_t *h,
                                                             const uint8_t *in, size_t len)
{
    size_t i;

    if (h->validated) {
        return OD_ZLIB_HEADER_OK;
    }
    for (i = 0u; i < len; ++i) {
        if (h->have == 0u) {
            h->cmf = in[i];
            h->have = 1u;
            continue;
        }
        {
            const od_zlib_header_result_t rc = od_zlib_header_check(h->cmf, in[i]);

            h->have = 2u;
            if (rc == OD_ZLIB_HEADER_OK) {
                h->validated = true;
            }
            return rc;
        }
    }
    return OD_ZLIB_HEADER_NEED_MORE;
}

#ifdef __cplusplus
}
#endif

#endif /* OD_ZLIB_HEADER_H */
