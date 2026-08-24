/* config_asm_test.c -- host tests for chunked CONFIG_WRITE reassembly (correctness review F3).
 *
 * Written against od_config_asm.h before the implementation existed. The boundary matrix is
 * the one the review specifies: totals 0, 200, 201, 399, 400, 401, 4000, 4001, 4096 and 4097,
 * plus truncated first frames, short middle chunks, excess final bytes, duplicate finals,
 * continuations without a start, and abandonment mid-transfer.
 *
 * THE INVARIANT THESE EXIST TO PROTECT is not "the parser is tidy" -- it is that a malformed
 * sequence never produces a COMPLETE, because COMPLETE is the only thing that lets a caller
 * write NVS. Every rejection case below asserts the result is REJECTED, which is the same
 * assertion as "storage did not change".
 */
#include "od_config_asm.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);  \
        }                                                                       \
    } while (0)

#define CASE(name) (g_case = (name))

static struct od_config_asm g_asm;
static uint8_t g_scratch[OD_CONFIG_MAX_SIZE + 512];

static void fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    for (uint32_t i = 0; i < n; ++i) {
        p[i] = (uint8_t)(seed + (i & 0xFFu));
    }
}

/* Build a legal chunked start frame: [total:2 LE][200 data bytes]. */
static uint16_t make_start(uint8_t *out, uint32_t total, uint8_t seed)
{
    out[0] = (uint8_t)(total & 0xFFu);
    out[1] = (uint8_t)((total >> 8) & 0xFFu);
    fill(out + 2, CONFIG_CHUNK_SIZE, seed);
    return (uint16_t)(2u + CONFIG_CHUNK_SIZE);
}

/* Drive a complete, well-formed transfer of `total` bytes. Returns the final result. */
static enum od_config_asm_result run_transfer(uint32_t total)
{
    uint16_t len = make_start(g_scratch, total, 0x11);
    enum od_config_asm_result r = od_config_asm_start(&g_asm, od_span_make(g_scratch, len));
    if (r != OD_CONFIG_ASM_ACCEPTED && r != OD_CONFIG_ASM_COMPLETE) {
        return r;
    }
    while (r == OD_CONFIG_ASM_ACCEPTED) {
        const uint32_t remaining = total - g_asm.received;
        const uint32_t n = (remaining > CONFIG_CHUNK_SIZE) ? CONFIG_CHUNK_SIZE : remaining;
        fill(g_scratch, n, 0x22);
        r = od_config_asm_chunk(&g_asm, od_span_make(g_scratch, (uint16_t)n));
    }
    return r;
}

/* ------------------------------------------------------------------------ start-frame shape --- */

static void test_start_shapes(void)
{
    CASE("empty start is rejected");
    od_config_asm_reset(&g_asm);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, 0)) == OD_CONFIG_ASM_REJECTED);

    CASE("single-frame payloads are not chunked");
    for (uint16_t len = 1; len <= CONFIG_CHUNK_SIZE; len += 43) {
        od_config_asm_reset(&g_asm);
        fill(g_scratch, len, 0x33);
        CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_SINGLE);
        CHECK(!g_asm.active);   /* a single frame must leave NO transfer in progress */
    }

    /* 201 is the header's documented short-first-chunk fallback. The old code made this an
     * active one-chunk transfer that was ACKed and then never saved -- a config write that
     * silently did nothing. */
    CASE("201-byte start is a single frame, not an orphaned transfer");
    od_config_asm_reset(&g_asm);
    fill(g_scratch, 201, 0x44);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, 201)) == OD_CONFIG_ASM_SINGLE);
    CHECK(!g_asm.active);

    /* The old code copied only the first 200 data bytes and silently dropped the rest.
     *
     * THE DECLARED TOTAL HERE IS DELIBERATELY VALID (400). An earlier version of this test left
     * whatever bytes happened to be in the scratch buffer, so the frame was rejected by the
     * TOTAL-bounds check rather than the LENGTH check -- it passed with the length check
     * deleted. Mutation testing caught that; a valid total makes the length rule the only thing
     * that can reject these. */
    CASE("start frames longer than 202 are rejected, not truncated");
    for (uint16_t len = 203; len < 260; len += 7) {
        od_config_asm_reset(&g_asm);
        (void)make_start(g_scratch, 400u, 0x4A);      /* valid total, valid first 200 bytes */
        fill(g_scratch + 202, (uint32_t)len - 202u, 0x4B);   /* ...and some excess after it */
        CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_REJECTED);
        CHECK(!g_asm.active);
    }
}

/* --------------------------------------------------------------------- declared-total bounds --- */

static void test_declared_total_bounds(void)
{
    /* A declared total that a single frame should have carried is nonsense in a chunked start.
     * The old code accepted 0 and 200 and produced meaningless expected counts. */
    CASE("declared totals of 0 and 200 are rejected at the start");
    const uint32_t bad_low[] = { 0u, 1u, CONFIG_CHUNK_SIZE };
    for (unsigned i = 0; i < sizeof bad_low / sizeof bad_low[0]; ++i) {
        od_config_asm_reset(&g_asm);
        uint16_t len = make_start(g_scratch, bad_low[i], 0x55);
        CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_REJECTED);
        CHECK(!g_asm.active);
    }

    CASE("oversized declared totals are rejected at the START, not later");
    const uint32_t bad_high[] = { OD_CONFIG_ASM_MAX_TRANSFERABLE + 1u,
                                  OD_CONFIG_MAX_SIZE, OD_CONFIG_MAX_SIZE + 1u, 65535u };
    for (unsigned i = 0; i < sizeof bad_high / sizeof bad_high[0]; ++i) {
        od_config_asm_reset(&g_asm);
        uint16_t len = make_start(g_scratch, bad_high[i], 0x66);
        CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_REJECTED);
        CHECK(!g_asm.active);
    }
}

/* ------------------------------------------------------------------------- the boundary matrix --- */

static void test_boundary_totals(void)
{
    /* Totals the review names. 4096 and 4097 are expected to FAIL today: MAX_CONFIG_CHUNKS is
     * 20, so 4000 is the real ceiling until the canonical header takes 21 (D4). Asserting the
     * limit rather than the aspiration is the point -- if someone raises the constant, this
     * test tells them which behaviour changed. */
    const struct { uint32_t total; int expect_complete; } cases[] = {
        { 201u,  1 }, { 399u,  1 }, { 400u,  1 }, { 401u, 1 },
        { 3999u, 1 }, { 4000u, 1 },
        { 4001u, 0 }, { 4096u, 0 }, { 4097u, 0 },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        CASE("boundary total");
        od_config_asm_reset(&g_asm);
        const enum od_config_asm_result r = run_transfer(cases[i].total);
        if (cases[i].expect_complete) {
            if (r != OD_CONFIG_ASM_COMPLETE) {
                printf("  (total %u expected COMPLETE, got %d)\n",
                       (unsigned)cases[i].total, (int)r);
            }
            CHECK(r == OD_CONFIG_ASM_COMPLETE);
            CHECK(g_asm.received == cases[i].total);
            CHECK(g_asm.total_size == cases[i].total);
        } else {
            if (r == OD_CONFIG_ASM_COMPLETE) {
                printf("  (total %u unexpectedly COMPLETED)\n", (unsigned)cases[i].total);
            }
            CHECK(r == OD_CONFIG_ASM_COMPLETE ? 0 : 1);
        }
    }

    CASE("the transferable ceiling is 4000, not OD_CONFIG_MAX_SIZE");
    CHECK(OD_CONFIG_ASM_MAX_TRANSFERABLE == 4000u);
    CHECK(OD_CONFIG_MAX_SIZE == 4096u);
}

/* ------------------------------------------------------------------------ malformed sequences --- */

static void test_excess_bytes_rejected(void)
{
    /* THE F3 HEADLINE CASE. Declared total 201, then a full 200-byte continuation. The old
     * code committed 400 bytes because the chunk count reached the expected two. */
    CASE("excess beyond the declared total is rejected (the 201+200 case)");
    od_config_asm_reset(&g_asm);
    uint16_t len = make_start(g_scratch, 201u, 0x77);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    CHECK(g_asm.received == CONFIG_CHUNK_SIZE);

    fill(g_scratch, CONFIG_CHUNK_SIZE, 0x88);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE)) == OD_CONFIG_ASM_REJECTED);
    CHECK(!g_asm.active);

    CASE("the same transfer completes with the correct 1-byte tail");
    od_config_asm_reset(&g_asm);
    len = make_start(g_scratch, 201u, 0x77);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    fill(g_scratch, 1u, 0x99);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, 1u)) == OD_CONFIG_ASM_COMPLETE);
    CHECK(g_asm.received == 201u);
}

static void test_short_middle_chunk_rejected(void)
{
    /* A non-final chunk must be a FULL 200 bytes. A short one that does not complete the total
     * means the client and the device disagree about the framing. */
    CASE("a short middle chunk is rejected");
    od_config_asm_reset(&g_asm);
    uint16_t len = make_start(g_scratch, 600u, 0xA1);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    fill(g_scratch, 50u, 0xA2);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, 50u)) == OD_CONFIG_ASM_REJECTED);
    CHECK(!g_asm.active);
}

static void test_continuation_without_start(void)
{
    CASE("a continuation with no active transfer is rejected");
    od_config_asm_reset(&g_asm);
    fill(g_scratch, CONFIG_CHUNK_SIZE, 0xB1);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE)) == OD_CONFIG_ASM_REJECTED);

    CASE("and again immediately after a completed transfer");
    od_config_asm_reset(&g_asm);
    CHECK(run_transfer(400u) == OD_CONFIG_ASM_COMPLETE);
    fill(g_scratch, CONFIG_CHUNK_SIZE, 0xB2);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE)) == OD_CONFIG_ASM_REJECTED);
}

static void test_duplicate_final_chunk(void)
{
    CASE("a duplicate final chunk is rejected, not re-committed");
    od_config_asm_reset(&g_asm);
    uint16_t len = make_start(g_scratch, 400u, 0xC1);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    fill(g_scratch, CONFIG_CHUNK_SIZE, 0xC2);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE)) == OD_CONFIG_ASM_COMPLETE);
    /* The transfer is over; a repeat of the final frame must not start a second commit. */
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE)) == OD_CONFIG_ASM_REJECTED);
}

static void test_bad_chunk_sizes(void)
{
    CASE("empty and over-long chunks are rejected");
    od_config_asm_reset(&g_asm);
    uint16_t len = make_start(g_scratch, 600u, 0xD1);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, 0u)) == OD_CONFIG_ASM_REJECTED);

    od_config_asm_reset(&g_asm);
    len = make_start(g_scratch, 600u, 0xD2);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE + 1u))
          == OD_CONFIG_ASM_REJECTED);
}

static void test_abandon_midway(void)
{
    CASE("a reset mid-transfer leaves nothing to commit");
    od_config_asm_reset(&g_asm);
    uint16_t len = make_start(g_scratch, 600u, 0xE1);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    od_config_asm_reset(&g_asm);
    CHECK(!g_asm.active);
    CHECK(g_asm.received == 0u);
    fill(g_scratch, CONFIG_CHUNK_SIZE, 0xE2);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE)) == OD_CONFIG_ASM_REJECTED);

    CASE("a new start after abandonment works normally");
    CHECK(run_transfer(400u) == OD_CONFIG_ASM_COMPLETE);
}

static void test_payload_bytes_are_exact(void)
{
    /* Reassembly must produce the bytes the client sent, in order. A test that only checks
     * lengths would pass over a memcpy with the wrong offset. */
    CASE("reassembled bytes match what was sent");
    od_config_asm_reset(&g_asm);
    const uint32_t total = 450u;

    uint8_t expect[512];
    fill(expect, CONFIG_CHUNK_SIZE, 0x11);
    fill(expect + CONFIG_CHUNK_SIZE, CONFIG_CHUNK_SIZE, 0x22);
    fill(expect + 2u * CONFIG_CHUNK_SIZE, total - 2u * CONFIG_CHUNK_SIZE, 0x33);

    uint16_t len = make_start(g_scratch, total, 0x11);
    CHECK(od_config_asm_start(&g_asm, od_span_make(g_scratch, len)) == OD_CONFIG_ASM_ACCEPTED);
    fill(g_scratch, CONFIG_CHUNK_SIZE, 0x22);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, CONFIG_CHUNK_SIZE)) == OD_CONFIG_ASM_ACCEPTED);
    fill(g_scratch, total - 2u * CONFIG_CHUNK_SIZE, 0x33);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(g_scratch, (uint16_t)(total - 2u * CONFIG_CHUNK_SIZE)))
          == OD_CONFIG_ASM_COMPLETE);
    CHECK(memcmp(g_asm.buffer, expect, total) == 0);
}

static void test_null_safety(void)
{
    CASE("null arguments are refused, not dereferenced");
    od_config_asm_reset(NULL);
    CHECK(od_config_asm_start(NULL, od_span_make(g_scratch, 10)) == OD_CONFIG_ASM_REJECTED);
    CHECK(od_config_asm_chunk(NULL, od_span_make(g_scratch, 10)) == OD_CONFIG_ASM_REJECTED);
    od_config_asm_reset(&g_asm);
    CHECK(od_config_asm_start(&g_asm, od_span_make(NULL, 10)) == OD_CONFIG_ASM_REJECTED);
    CHECK(od_config_asm_chunk(&g_asm, od_span_make(NULL, 10)) == OD_CONFIG_ASM_REJECTED);
}

int main(void)
{
    test_start_shapes();
    test_declared_total_bounds();
    test_boundary_totals();
    test_excess_bytes_rejected();
    test_short_middle_chunk_rejected();
    test_continuation_without_start();
    test_duplicate_final_chunk();
    test_bad_chunk_sizes();
    test_abandon_midway();
    test_payload_bytes_are_exact();
    test_null_safety();

    printf("config_asm: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
