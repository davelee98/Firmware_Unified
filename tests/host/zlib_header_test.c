/* od_zlib_header — the one zlib-header rule both inflate engines apply.
 *
 * The window bound is a WIRE CONTRACT, not a buffer size: a target that accepts a wider stream
 * than the rest of the fleet is a divergence no host can interrogate. The portable engine and the
 * ESP32 tinfl adapter reach this rule from opposite directions -- one parses the header itself,
 * the other must refuse before its engine parses it -- so both entry points are exercised here.
 */

#include "od_zlib_header.h"
#include "od_check.h"

/* Build a well-formed zlib header for a given window: CINFO = bits - 8, CM = 8, and FCHECK
 * chosen so the pair is a multiple of 31. FDICT stays clear. */
static void make_header(unsigned window_bits, uint8_t *cmf, uint8_t *flg)
{
    unsigned rem;

    *cmf = (uint8_t)(((window_bits - 8u) << 4) | 8u);
    rem = (256u * (unsigned)*cmf) % 31u;
    *flg = (uint8_t)((31u - rem) % 31u);
}

static void test_make_header_is_valid_by_construction(void)
{
    CASE("the fixture builds headers RFC 1950 accepts");
    for (unsigned bits = 8u; bits <= 15u; ++bits) {
        uint8_t cmf, flg;

        make_header(bits, &cmf, &flg);
        CHECK(((256u * cmf + flg) % 31u) == 0u);
        CHECK((cmf & 0x0Fu) == 8u);
        CHECK((flg & 0x20u) == 0u);
    }
}

static void test_window_bound(void)
{
    CASE("a window at or under the build limit is accepted, wider is refused");
    for (unsigned bits = 8u; bits <= 15u; ++bits) {
        uint8_t cmf, flg;
        od_zlib_header_result_t rc;

        make_header(bits, &cmf, &flg);
        rc = od_zlib_header_check(cmf, flg);
        if (bits <= OPENDISPLAY_ZLIB_WINDOW_BITS) {
            CHECK(rc == OD_ZLIB_HEADER_OK);
        } else {
            CHECK(rc == OD_ZLIB_HEADER_WINDOW_TOO_BIG);
        }
    }

    /* The default build limit is 9. These are the widths a 4096-byte tinfl ring would have
     * accepted on its own -- the divergence this rule exists to close. */
    CASE("10..12-bit streams are refused even where an engine's own ring would hold them");
    for (unsigned bits = 10u; bits <= 12u; ++bits) {
        uint8_t cmf, flg;

        make_header(bits, &cmf, &flg);
        CHECK(od_zlib_header_check(cmf, flg) ==
              (bits <= OPENDISPLAY_ZLIB_WINDOW_BITS ? OD_ZLIB_HEADER_OK
                                                    : OD_ZLIB_HEADER_WINDOW_TOO_BIG));
    }
}

static void test_malformed(void)
{
    uint8_t cmf, flg;

    CASE("a bad check value is refused");
    make_header(9u, &cmf, &flg);
    CHECK(od_zlib_header_check(cmf, (uint8_t)(flg ^ 0x01u)) == OD_ZLIB_HEADER_BAD);

    CASE("a compression method other than deflate is refused");
    make_header(9u, &cmf, &flg);
    CHECK(od_zlib_header_check((uint8_t)((cmf & 0xF0u) | 7u), flg) == OD_ZLIB_HEADER_BAD);

    CASE("a preset dictionary is refused");
    /* Set FDICT and re-fix FCHECK, so the ONLY thing wrong is the dictionary bit. */
    {
        uint8_t d_cmf = (uint8_t)((1u << 4) | 8u);   /* 9-bit window */
        unsigned base = (256u * (unsigned)d_cmf) + 0x20u;
        uint8_t d_flg = (uint8_t)(0x20u + ((31u - (base % 31u)) % 31u));

        CHECK(((256u * d_cmf + d_flg) % 31u) == 0u);   /* well-formed apart from FDICT */
        CHECK((d_flg & 0x20u) != 0u);
        CHECK(od_zlib_header_check(d_cmf, d_flg) == OD_ZLIB_HEADER_BAD);
    }
}

/* The streaming form: the adapter that needs this cannot buffer the stream, so the two header
 * bytes may arrive in separate pushes, or behind an empty one. */
static void test_observe_across_pushes(void)
{
    uint8_t cmf, flg;
    od_zlib_header_t h;

    make_header(OPENDISPLAY_ZLIB_WINDOW_BITS, &cmf, &flg);

    CASE("both bytes in one push");
    od_zlib_header_reset(&h);
    {
        const uint8_t in[] = { cmf, flg, 0x00u, 0x11u };
        CHECK(od_zlib_header_observe(&h, in, sizeof in) == OD_ZLIB_HEADER_OK);
    }

    CASE("one byte per push");
    od_zlib_header_reset(&h);
    CHECK(od_zlib_header_observe(&h, &cmf, 1u) == OD_ZLIB_HEADER_NEED_MORE);
    CHECK(od_zlib_header_observe(&h, &flg, 1u) == OD_ZLIB_HEADER_OK);

    CASE("an empty push before the header is not a verdict");
    od_zlib_header_reset(&h);
    CHECK(od_zlib_header_observe(&h, NULL, 0u) == OD_ZLIB_HEADER_NEED_MORE);
    CHECK(od_zlib_header_observe(&h, &cmf, 1u) == OD_ZLIB_HEADER_NEED_MORE);
    CHECK(od_zlib_header_observe(&h, NULL, 0u) == OD_ZLIB_HEADER_NEED_MORE);
    CHECK(od_zlib_header_observe(&h, &flg, 1u) == OD_ZLIB_HEADER_OK);

    CASE("once validated, later payload never re-triggers the rule");
    for (unsigned i = 0; i < 8u; ++i) {
        const uint8_t junk[] = { 0xFFu, 0xFFu, 0xFFu };
        CHECK(od_zlib_header_observe(&h, junk, sizeof junk) == OD_ZLIB_HEADER_OK);
    }

    CASE("an over-wide window split across pushes is still refused");
    if (OPENDISPLAY_ZLIB_WINDOW_BITS < 15u) {
        uint8_t w_cmf, w_flg;

        make_header((unsigned)OPENDISPLAY_ZLIB_WINDOW_BITS + 1u, &w_cmf, &w_flg);
        od_zlib_header_reset(&h);
        CHECK(od_zlib_header_observe(&h, &w_cmf, 1u) == OD_ZLIB_HEADER_NEED_MORE);
        CHECK(od_zlib_header_observe(&h, &w_flg, 1u) == OD_ZLIB_HEADER_WINDOW_TOO_BIG);
        /* A refusal is not sticky-OK: re-observing must not report success. */
        CHECK(od_zlib_header_observe(&h, &w_flg, 1u) != OD_ZLIB_HEADER_OK);
    }
}

int main(void)
{
    test_make_header_is_valid_by_construction();
    test_window_bound();
    test_malformed();
    test_observe_across_pushes();

    return OD_CHECK_REPORT_NONEMPTY("zlib_header", 40);
}
