/* txq_test.c -- shared/core/od_txq.c against a scripted radio.
 *
 * The fake supplies od_hal_radio_{send,tag_is_live} at LINK time, as a target does: shared/ binds
 * its HAL at link time by design, so there is no injection seam and this exercises real linkage.
 *
 * What a green run does NOT prove: that a real stack's busy condition maps to RETRY rather than
 * ERROR, or that a NimBLE/Zephyr notify actually reaches air. Those need the targets and hardware.
 * What it does prove is the policy: accounting, ordering, and what happens to a queue when a link
 * dies underneath it.
 */

#include "od_txq.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)

#define CASE(name) (g_case = (name))

/* ------------------------------------------------------------------------------ fake radio --- */

#define SENT_MAX 64u

static od_radio_result_t g_next_result;      /* what send() returns, unless scripted below */
static od_radio_result_t g_script[SENT_MAX]; /* per-call overrides, consumed in order */
static unsigned g_script_len;
static unsigned g_send_calls;

static uint32_t g_dead_tag;                  /* tag_is_live() returns false for this one */
static bool     g_dead_tag_set;

/* Everything send() was asked to deliver, in order, so ORDERING is assertable and not merely
 * "the right number of things happened". */
static struct { od_origin_t origin; uint32_t tag; uint16_t len; uint8_t first; } g_sent[SENT_MAX];
static unsigned g_sent_n;

static void fake_reset(void)
{
    g_next_result = OD_RADIO_SENT;
    memset(g_script, 0, sizeof g_script);
    g_script_len = 0u;
    g_send_calls = 0u;
    g_dead_tag = 0u;
    g_dead_tag_set = false;
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    od_txq_reset();
}

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    const unsigned n = g_send_calls++;

    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].origin = origin;
        g_sent[g_sent_n].tag = tag;
        g_sent[g_sent_n].len = len;
        g_sent[g_sent_n].first = (len > 0u) ? frame[0] : 0u;
        ++g_sent_n;
    }
    if (n < g_script_len) {
        return g_script[n];
    }
    return g_next_result;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin;
    return !(g_dead_tag_set && tag == g_dead_tag);
}

/* od_txq's drop seam. Counted so a test can assert that a discarded entry was REPORTED, not just
 * that it vanished -- the difference between a diagnosable failure and a silent one. */
static unsigned g_dropped;
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
    ++g_dropped;
}

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE1 = { OD_ORIGIN_BLE, 1u };
static const od_reply_t BLE2 = { OD_ORIGIN_BLE, 2u };

/* Queue one frame whose first byte is `marker`, so g_sent can identify it later. */
static od_txq_status_t queue_one(od_tx_reservation_t *r, const od_reply_t *rp, uint8_t marker)
{
    uint8_t frame[4];

    frame[0] = marker;
    frame[1] = 0x00u;
    frame[2] = 0x00u;
    frame[3] = 0x00u;
    return od_txq_commit(r, rp, frame, sizeof frame);
}

/* ----------------------------------------------------------------------------- reservation --- */

static void test_reservation_accounting(void)
{
    od_tx_reservation_t a, b;

    CASE("reserve claims units; release returns only the unused ones");
    fake_reset();
    CHECK(od_txq_reserve(3u, &a) == OD_TXQ_OK);
    CHECK(a.remaining == 3u);
    CHECK(od_txq_reserved() == 3u);
    CHECK(queue_one(&a, &BLE1, 0xA0u) == OD_TXQ_OK);
    CHECK(a.remaining == 2u);
    CHECK(od_txq_reserved() == 2u);      /* the spent unit is now a queued entry, not a claim */
    CHECK(od_txq_depth() == 1u);
    od_txq_release(&a);
    CHECK(od_txq_reserved() == 0u);
    CHECK(a.remaining == 0u);
    CHECK(od_txq_depth() == 1u);         /* release does not un-queue what was committed */

    CASE("release is idempotent and NULL-safe");
    od_txq_release(&a);
    od_txq_release(NULL);
    CHECK(od_txq_reserved() == 0u);

    CASE("a second token cannot see the first token's units");
    fake_reset();
    CHECK(od_txq_reserve(2u, &a) == OD_TXQ_OK);
    CHECK(od_txq_reserve(2u, &b) == OD_TXQ_OK);
    CHECK(od_txq_reserved() == 4u);
    CHECK(queue_one(&a, &BLE1, 0xA1u) == OD_TXQ_OK);
    CHECK(queue_one(&a, &BLE1, 0xA2u) == OD_TXQ_OK);
    CHECK(a.remaining == 0u);
    /* THE POINT OF THE TOKEN: a's third reply is refused even though b still holds capacity. */
    CHECK(queue_one(&a, &BLE1, 0xA3u) == OD_TXQ_INVARIANT);
    CHECK(od_txq_depth() == 2u);
    CHECK(b.remaining == 2u);            /* b's claim is untouched by a's overrun */

    CASE("reserve refuses rather than overcommitting, and refuses before any side effect");
    fake_reset();
    {
        od_tx_reservation_t big;
        const uint8_t usable = (uint8_t)(OD_TXQ_SLOTS - 1u);
        CHECK(od_txq_reserve(usable, &big) == OD_TXQ_OK);
        CHECK(od_txq_reserve(1u, &a) == OD_TXQ_FULL);
        CHECK(a.remaining == 0u);
        CHECK(od_txq_reserved() == usable);   /* the refused reserve claimed nothing */
        od_txq_release(&big);
        CHECK(od_txq_reserve(1u, &a) == OD_TXQ_OK);
    }

    CASE("bad arguments are invariant failures, not silent successes");
    fake_reset();
    CHECK(od_txq_reserve(0u, &a) == OD_TXQ_INVARIANT);
    CHECK(od_txq_reserve(1u, NULL) == OD_TXQ_INVARIANT);
    CHECK(od_txq_reserve(1u, &a) == OD_TXQ_OK);
    CHECK(od_txq_commit(&a, &BLE1, NULL, 4u) == OD_TXQ_INVARIANT);
    CHECK(od_txq_commit(&a, NULL, (const uint8_t *)"x", 1u) == OD_TXQ_INVARIANT);
    CHECK(od_txq_commit(NULL, &BLE1, (const uint8_t *)"x", 1u) == OD_TXQ_INVARIANT);
    CHECK(a.remaining == 1u);            /* none of those spent the unit */
}

/* --------------------------------------------------------------------------------- framing --- */

static void test_ble_value_ceiling(void)
{
    od_tx_reservation_t r;
    static uint8_t frame[OD_TX_FRAME_MAX + 4];

    memset(frame, 0x5A, sizeof frame);

    CASE("BLE accepts exactly 253 and refuses 254 -- storage width is not permission");
    fake_reset();
    CHECK(od_txq_reserve(4u, &r) == OD_TXQ_OK);
    CHECK(od_txq_commit(&r, &BLE1, frame, OD_TXQ_VALUE_MAX_BLE) == OD_TXQ_OK);
    CHECK(od_txq_commit(&r, &BLE1, frame, OD_TXQ_VALUE_MAX_BLE + 1u) == OD_TXQ_TOO_LARGE);
    /* 256 fits the entry and is still refused: OD_TX_FRAME_MAX is how wide a slot is, not what
     * ATT will carry. Getting this wrong moves the failure to the radio, where it is a dropped
     * response rather than a refused one. */
    CHECK(od_txq_commit(&r, &BLE1, frame, OD_TX_FRAME_MAX) == OD_TXQ_TOO_LARGE);
    CHECK(od_txq_commit(&r, &BLE1, frame, OD_TX_FRAME_MAX + 1u) == OD_TXQ_TOO_LARGE);

    CASE("a refused length spends no unit and queues nothing");
    CHECK(r.remaining == 3u);            /* only the 253-byte commit spent one */
    CHECK(od_txq_depth() == 1u);
}

/* ----------------------------------------------------------------------------------- drain --- */

static void test_drain_results(void)
{
    od_tx_reservation_t r;

    CASE("SENT retires entries in FIFO order");
    fake_reset();
    CHECK(od_txq_reserve(3u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x33u) == OD_TXQ_OK);
    CHECK(od_txq_process() == 3u);
    CHECK(od_txq_depth() == 0u);
    CHECK(g_sent_n == 3u);
    CHECK(g_sent[0].first == 0x11u);
    CHECK(g_sent[1].first == 0x22u);
    CHECK(g_sent[2].first == 0x33u);

    CASE("RETRY keeps the entry and STOPS the pass -- no reordering past it");
    fake_reset();
    CHECK(od_txq_reserve(3u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x33u) == OD_TXQ_OK);
    g_script[0] = OD_RADIO_SENT;
    g_script[1] = OD_RADIO_RETRY;
    g_script_len = 2u;
    g_next_result = OD_RADIO_RETRY;
    CHECK(od_txq_process() == 1u);
    CHECK(od_txq_depth() == 2u);         /* 0x22 and 0x33 both still queued */
    CHECK(g_sent_n == 2u);               /* 0x33 was never even offered */
    CHECK(g_sent[1].first == 0x22u);
    /* Once the transport frees up, the SAME entry goes first. */
    g_script_len = 0u;
    g_next_result = OD_RADIO_SENT;
    CHECK(od_txq_process() == 2u);
    CHECK(g_sent[2].first == 0x22u);
    CHECK(g_sent[3].first == 0x33u);

    CASE("ERROR retires one entry and continues -- never retried, never a teardown");
    fake_reset();
    CHECK(od_txq_reserve(3u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x33u) == OD_TXQ_OK);
    g_script[1] = OD_RADIO_ERROR;
    g_script_len = 2u;
    g_next_result = OD_RADIO_SENT;
    CHECK(od_txq_process() == 3u);
    CHECK(od_txq_depth() == 0u);
    CHECK(g_send_calls == 3u);           /* the failed entry was offered exactly once */
    CHECK(g_sent[2].first == 0x33u);     /* and the one behind it still went */

    CASE("GONE drops every entry for that tag and keeps the others in order");
    fake_reset();
    CHECK(od_txq_reserve(4u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE2, 0xAAu) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE2, 0xBBu) == OD_TXQ_OK);
    g_script[0] = OD_RADIO_GONE;         /* tag 1 dies on its first entry */
    g_script_len = 1u;
    g_next_result = OD_RADIO_SENT;
    CHECK(od_txq_process() == 2u);       /* only tag 2's two entries retire */
    CHECK(od_txq_depth() == 0u);
    CHECK(g_sent_n == 3u);               /* the GONE attempt, then tag 2's two */
    CHECK(g_sent[1].first == 0xAAu);
    CHECK(g_sent[2].first == 0xBBu);     /* survivors kept their relative order */
}

static void test_dead_tag(void)
{
    od_tx_reservation_t r;

    CASE("committing to a dead tag is refused and does not queue");
    fake_reset();
    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    g_dead_tag = 1u;
    g_dead_tag_set = true;
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_GONE);
    CHECK(od_txq_depth() == 0u);
    /* The unit is spent even so: the dispatch is over, and handing it back would let a doomed
     * reply free capacity it never used. */
    CHECK(r.remaining == 1u);
    CHECK(od_txq_reserved() == 1u);

    CASE("a tag that dies while queued is dropped without a send attempt");
    fake_reset();
    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE2, 0xAAu) == OD_TXQ_OK);
    g_dead_tag = 1u;
    g_dead_tag_set = true;
    CHECK(od_txq_process() == 1u);
    CHECK(od_txq_depth() == 0u);
    CHECK(g_send_calls == 1u);           /* only tag 2 was offered to the radio */
    CHECK(g_sent[0].first == 0xAAu);
}

/* ----------------------------------------------------------------------------------- flush --- */

static void test_flush(void)
{
    od_tx_reservation_t r;

    CASE("flush on an empty queue is OK regardless of the clock");
    fake_reset();
    CHECK(od_txq_flush(1000u, 500u) == OD_TXQ_OK);

    CASE("flush drains and reports OK");
    fake_reset();
    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(od_txq_flush(1000u, 1500u) == OD_TXQ_OK);
    CHECK(od_txq_depth() == 0u);

    CASE("ON EXPIRY ENTRIES STAY QUEUED -- late beats dropped");
    fake_reset();
    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(od_txq_flush(1500u, 1500u) == OD_TXQ_TIMEOUT);   /* exactly at the deadline */
    CHECK(od_txq_depth() == 2u);
    CHECK(g_send_calls == 0u);           /* expired means it did not even try */
    CHECK(od_txq_flush(2000u, 1500u) == OD_TXQ_TIMEOUT);   /* past it */
    CHECK(od_txq_depth() == 2u);
    /* And they are still deliverable afterwards, which is the whole point of not dropping. */
    CHECK(od_txq_flush(1000u, 1500u) == OD_TXQ_OK);
    CHECK(od_txq_depth() == 0u);


    CASE("the deadline comparison survives the uint32_t rollover");
    fake_reset();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    /* now just before wrap, deadline just after: naive `now >= deadline` reads this as expired. */
    CHECK(od_txq_flush(0xFFFFF000u, 0x00000100u) == OD_TXQ_OK);
    CHECK(od_txq_depth() == 0u);
}

/* ----------------------------------------------------------------------------------- reset --- */

static void test_reset(void)
{
    od_tx_reservation_t r;

    CASE("reset clears entries and claims together");
    fake_reset();
    CHECK(od_txq_reserve(3u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(od_txq_depth() == 1u);
    CHECK(od_txq_reserved() == 2u);
    od_txq_reset();
    CHECK(od_txq_depth() == 0u);
    CHECK(od_txq_reserved() == 0u);
    /* A TOKEN MUST NOT OUTLIVE THE RESET THAT INVALIDATED IT. This case previously asserted the
     * opposite of the requirement stated on this very line -- it recorded the defect as intended
     * behaviour. A stale commit decrements a reserved count reset already zeroed, wrapping it to
     * 255, after which every reserve returns FULL until the next reset. */
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_INVARIANT);
    CHECK(od_txq_depth() == 0u);
    CHECK(od_txq_reserved() == 0u);

    CASE("and releasing a stale token does not give units back to the new queue");
    od_txq_release(&r);
    CHECK(od_txq_reserved() == 0u);
    {
        od_tx_reservation_t fresh;
        CHECK(od_txq_reserve((uint8_t)(OD_TXQ_SLOTS - 1u), &fresh) == OD_TXQ_OK);
        od_txq_release(&fresh);
    }
    od_txq_reset();
}

static void test_flush_busy_vs_timeout(void)
{
    od_tx_reservation_t r;

    CASE("an unwritable transport BEFORE the deadline is BUSY, not TIMEOUT");
    /* The two must be distinguishable. A caller reading TIMEOUT at face value gives up and starts
     * the panel refresh; if the radio recovers a moment later the END ack lands after the refresh
     * has begun, which is precisely what the barrier exists to prevent. */
    fake_reset();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    g_next_result = OD_RADIO_RETRY;
    CHECK(od_txq_flush(1000u, 1500u) == OD_TXQ_BUSY);
    CHECK(od_txq_depth() == 1u);

    CASE("and once it recovers within the deadline the frame goes");
    g_next_result = OD_RADIO_SENT;
    CHECK(od_txq_flush(1010u, 1500u) == OD_TXQ_OK);
    CHECK(od_txq_depth() == 0u);
    CHECK(g_sent[0].first == 0x11u);

    CASE("only a reached deadline is TIMEOUT");
    fake_reset();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(od_txq_flush(1500u, 1500u) == OD_TXQ_TIMEOUT);
    CHECK(od_txq_depth() == 1u);
    CHECK(g_send_calls == 0u);
}

static void test_error_and_gone_are_reported(void)
{
    od_tx_reservation_t r;

    CASE("a permanent refusal is reported, not silently swallowed");
    fake_reset();
    g_dropped = 0u;
    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    g_script[0] = OD_RADIO_ERROR;
    g_script_len = 1u;
    g_next_result = OD_RADIO_SENT;
    CHECK(od_txq_process() == 2u);
    CHECK(g_dropped == 1u);           /* exactly the one that could never be sent */

    CASE("and so is every frame lost to a dead link");
    fake_reset();
    g_dropped = 0u;
    CHECK(od_txq_reserve(3u, &r) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x11u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE1, 0x22u) == OD_TXQ_OK);
    CHECK(queue_one(&r, &BLE2, 0xAAu) == OD_TXQ_OK);
    g_dead_tag = 1u;
    g_dead_tag_set = true;
    CHECK(od_txq_process() == 1u);
    CHECK(g_dropped == 2u);           /* both of tag 1's, individually */
}

/* --------------------------------------------------------------------------------- wrapping --- */

static void test_ring_wraps(void)
{
    od_tx_reservation_t r;
    unsigned k;

    CASE("the ring survives many fill/drain cycles without drifting");
    fake_reset();
    for (k = 0; k < 200u; ++k) {
        CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
        CHECK(queue_one(&r, &BLE1, (uint8_t)(k & 0xFFu)) == OD_TXQ_OK);
        CHECK(queue_one(&r, &BLE1, (uint8_t)((k + 1u) & 0xFFu)) == OD_TXQ_OK);
        CHECK(od_txq_depth() == 2u);
        CHECK(od_txq_process() == 2u);
        CHECK(od_txq_depth() == 0u);
        CHECK(od_txq_reserved() == 0u);
        g_sent_n = 0u;                   /* keep the capture window from overflowing */
        g_send_calls = 0u;
    }
}

int main(void)
{
    test_reservation_accounting();
    test_ble_value_ceiling();
    test_drain_results();
    test_dead_tag();
    test_flush();
    test_reset();
    test_flush_busy_vs_timeout();
    test_error_and_gone_are_reported();
    test_ring_wraps();

    printf("txq: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
