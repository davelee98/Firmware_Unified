/* span_test.c -- host tests for od_span_t, the view every shared/ buffer argument travels as.
 *
 * WHY A TYPE THIS SMALL GETS ITS OWN SUITE. od_span_split() is now the single place the config
 * walk's "does this packet fit" arithmetic is written -- the check that used to appear fifteen
 * times on the pre-auth attack surface. A defect in it is a defect in every caller at once, and
 * it is exactly the kind of function whose callers all look correct while it is wrong.
 *
 * The cases that matter are therefore the ones a caller cannot see: SIZE_MAX lengths that would
 * wrap an addition, the malformed (NULL, n>0) span, and the difference between the checked
 * split and the saturating take/drop -- because picking the wrong one of those is how a
 * truncation turns back into a silently shorter read.
 */
#include "od_span.h"

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

static const uint8_t BUF[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static uint8_t MUT_BUF[8];

/* ------------------------------------------------------------------------------ well-formed --- */

static void test_valid(void)
{
    CASE("a pointer with a length is well-formed");
    CHECK(od_span_valid(od_span_make(BUF, sizeof BUF)));
    CHECK(od_span_valid(od_span_make(BUF, 0u)));

    CASE("NULL is well-formed only at zero length");
    CHECK(od_span_valid(od_span_none()));
    CHECK(od_span_valid(od_span_make(NULL, 0u)));
    CHECK(!od_span_valid(od_span_make(NULL, 1u)));
    CHECK(!od_span_valid(od_span_make(NULL, SIZE_MAX)));

    CASE("od_span_none is empty and points nowhere");
    CHECK(od_span_is_empty(od_span_none()));
    CHECK(od_span_none().p == NULL);
    CHECK(od_span_none().n == 0u);

    CASE("empty is about length, not about the pointer");
    CHECK(od_span_is_empty(od_span_make(BUF, 0u)));
    CHECK(!od_span_is_empty(od_span_make(BUF, 1u)));
}

/* ------------------------------------------------------------------------------------- has --- */

static void test_has(void)
{
    const od_span_t s = od_span_make(BUF, sizeof BUF);

    CASE("has answers about the length it was given");
    CHECK(od_span_has(s, 0u));
    CHECK(od_span_has(s, 8u));
    CHECK(!od_span_has(s, 9u));

    /* The reason has() and split() compare instead of adding. A caller asking about a length
     * near SIZE_MAX is asking about a declared size from the wire; an implementation that
     * computed `p + n` or `n <= end - start` with the wrong operand order would answer yes. */
    CASE("an absurd length cannot wrap into a yes");
    CHECK(!od_span_has(s, SIZE_MAX));
    CHECK(!od_span_has(s, SIZE_MAX - 4u));

    CASE("a malformed span holds nothing, not even zero bytes worth of trust");
    CHECK(!od_span_has(od_span_make(NULL, 4u), 1u));
    CHECK(!od_span_has(od_span_make(NULL, 4u), 0u));

    CASE("an empty but well-formed span holds exactly zero");
    CHECK(od_span_has(od_span_none(), 0u));
    CHECK(!od_span_has(od_span_none(), 1u));
}

/* ------------------------------------------------------------------------------- mutable --- */

static void test_mutable(void)
{
    od_mut_span_t s = od_mut_span_make(MUT_BUF, sizeof MUT_BUF);
    od_span_t read_only;

    CASE("a mutable span carries writable storage and its bound together");
    CHECK(od_mut_span_valid(s));
    CHECK(s.p == MUT_BUF && s.n == sizeof MUT_BUF);
    s.p[3] = 0xa5u;
    CHECK(MUT_BUF[3] == 0xa5u);

    CASE("NULL is well-formed for a mutable span only at zero length");
    CHECK(od_mut_span_valid(od_mut_span_none()));
    CHECK(!od_mut_span_valid(od_mut_span_make(NULL, 1u)));

    CASE("a mutable span converts to a read-only view without changing its bound");
    read_only = od_mut_span_const(s);
    CHECK(read_only.p == MUT_BUF && read_only.n == sizeof MUT_BUF);
}

/* ----------------------------------------------------------------------------------- split --- */

static void test_split(void)
{
    const od_span_t s = od_span_make(BUF, sizeof BUF);
    od_span_t head, tail;

    CASE("a split cuts exactly n bytes off the front");
    CHECK(od_span_split(s, 3u, &head, &tail));
    CHECK(head.p == BUF && head.n == 3u);
    CHECK(tail.p == BUF + 3 && tail.n == 5u);

    CASE("splitting the whole span leaves an empty tail that still points past the end");
    CHECK(od_span_split(s, 8u, &head, &tail));
    CHECK(head.n == 8u);
    CHECK(tail.n == 0u && tail.p == BUF + 8);

    CASE("a zero-length split is legal and moves nothing");
    CHECK(od_span_split(s, 0u, &head, &tail));
    CHECK(head.n == 0u && head.p == BUF);
    CHECK(tail.n == 8u && tail.p == BUF);

    /* THE ONE THAT MATTERS. A short buffer must be a false the caller has to branch on, not a
     * quietly shorter span it can keep walking. This is the truncated-config case. */
    CASE("a short span fails the split and writes nothing");
    head = od_span_make(BUF, 99u);
    tail = od_span_make(BUF, 99u);
    CHECK(!od_span_split(s, 9u, &head, &tail));
    CHECK(head.n == 99u && tail.n == 99u);      /* outputs untouched on failure */

    CASE("an absurd split length fails rather than wrapping");
    CHECK(!od_span_split(s, SIZE_MAX, &head, &tail));

    CASE("a malformed span cannot be split at all");
    CHECK(!od_span_split(od_span_make(NULL, 4u), 1u, &head, &tail));

    CASE("either output may be omitted");
    CHECK(od_span_split(s, 2u, NULL, &tail));
    CHECK(tail.n == 6u);
    CHECK(od_span_split(s, 2u, &head, NULL));
    CHECK(head.n == 2u);
    CHECK(od_span_split(s, 2u, NULL, NULL));

    /* od_config_tlv_walk() and od_config_asm_start() both do this, so it is pinned here rather
     * than left to the reader: the span is passed BY VALUE, so writing the tail back over the
     * input is reading a copy, not the destination. */
    CASE("splitting a span in place is safe");
    tail = s;
    CHECK(od_span_split(tail, 2u, &head, &tail));
    CHECK(head.p == BUF && head.n == 2u);
    CHECK(tail.p == BUF + 2 && tail.n == 6u);

    CASE("an empty span splits to nothing without forming NULL + 0");
    CHECK(od_span_split(od_span_none(), 0u, &head, &tail));
    CHECK(head.p == NULL && head.n == 0u);
    CHECK(tail.p == NULL && tail.n == 0u);
}

/* ------------------------------------------------------------------------------- take/drop --- */

static void test_take_drop(void)
{
    const od_span_t s = od_span_make(BUF, sizeof BUF);

    CASE("take keeps the front");
    CHECK(od_span_take(s, 3u).p == BUF);
    CHECK(od_span_take(s, 3u).n == 3u);
    CHECK(od_span_take(s, 0u).n == 0u);

    CASE("drop skips the front");
    CHECK(od_span_drop(s, 3u).p == BUF + 3);
    CHECK(od_span_drop(s, 3u).n == 5u);
    CHECK(od_span_drop(s, 0u).p == BUF);
    CHECK(od_span_drop(s, 0u).n == 8u);

    /* SATURATING, and this is the property that makes them wrong for parsing untrusted lengths:
     * an over-long take is a shorter span, not a failure. od_config_tlv.c uses them only after
     * an explicit length check, and the header says so. Pinned here so a change to clamping
     * behaviour cannot pass silently. */
    CASE("take and drop clamp instead of failing");
    CHECK(od_span_take(s, 99u).n == 8u);
    CHECK(od_span_take(s, SIZE_MAX).n == 8u);
    CHECK(od_span_drop(s, 99u).n == 0u);
    CHECK(od_span_drop(s, SIZE_MAX).n == 0u);

    CASE("dropping everything lands one past the end, never before the start");
    CHECK(od_span_drop(s, 8u).n == 0u);
    CHECK(od_span_drop(s, 8u).p == BUF + 8);
    CHECK(od_span_drop(s, 99u).p == BUF + 8);

    CASE("a malformed span yields nothing from either");
    CHECK(od_span_take(od_span_make(NULL, 4u), 1u).n == 0u);
    CHECK(od_span_take(od_span_make(NULL, 4u), 1u).p == NULL);
    CHECK(od_span_drop(od_span_make(NULL, 4u), 1u).n == 0u);
    CHECK(od_span_drop(od_span_make(NULL, 4u), 1u).p == NULL);

    CASE("an empty span survives both without forming NULL + 0");
    CHECK(od_span_drop(od_span_none(), 0u).p == NULL);
    CHECK(od_span_drop(od_span_none(), 4u).p == NULL);
    CHECK(od_span_take(od_span_none(), 4u).p == NULL);
}

/* ------------------------------------------------------------------------------ round trip --- */

static void test_walk_shape(void)
{
    /* The shape every shared/ walker has: consume a header, then a body, then repeat, with the
     * remainder carried in one value. Written out once here so the primitives are shown solving
     * the problem they were added for, not only in isolation. */
    od_span_t rest = od_span_make(BUF, sizeof BUF);
    od_span_t hdr, body;
    unsigned records = 0;

    CASE("split composes into a walk that cannot outrun its bound");
    while (od_span_split(rest, 1u, &hdr, &rest)) {
        const size_t declared = hdr.p[0];       /* the byte's value IS the body length here */
        if (!od_span_split(rest, declared, &body, &rest)) {
            break;                              /* truncated -- the caller's branch */
        }
        records++;
    }
    /* BUF is 0,1,2,3,...: record 0 declares 0 bytes, record 1 declares 1 byte (the 2), record 2
     * declares 3 bytes (4,5,6), then a 7-byte body is declared with nothing left. */
    CHECK(records == 3u);
    CHECK(rest.n == 0u);
    CHECK(rest.p == BUF + 8);
}

int main(void)
{
    test_valid();
    test_has();
    test_mutable();
    test_split();
    test_take_drop();
    test_walk_shape();

    printf("span: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
