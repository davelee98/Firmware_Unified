/* rxq_test.c -- shared/core/od_rxq.c
 *
 * The reporting callback is also a deterministic pre-publication barrier for the reset race: the
 * producer has selected a slot but has not copied or release-published it when the consumer resets.
 * That pins the legal concurrent case without a timing-dependent stress loop. The suite otherwise
 * covers FIFO discipline, full-ring refusal, stale-tag discard, and the reporting seam.
 */

#include "od_rxq.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const char *g_case = "";
static unsigned g_checks;
static unsigned g_fails;

#define CHECK(expr)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(expr)) {                                                                \
            ++g_fails;                                                                \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #expr);        \
        }                                                                             \
    } while (0)
#define CASE(name) (g_case = (name))

/* ------------------------------------------------------------------- the reporting seam --- */

#define REPORT_MAX 128
static struct {
    od_rxq_event_t ev;
    uint16_t       len;
    uint8_t        depth;
    uint8_t        first;      /* frame[0], to prove the report sees the arriving bytes */
} g_rep[REPORT_MAX];
static unsigned g_rep_n;

static pthread_mutex_t g_arrival_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_arrival_cond = PTHREAD_COND_INITIALIZER;
static bool g_block_arrival;
static bool g_arrival_entered;
static bool g_release_arrival;

void od_rxq_app_report(od_rxq_event_t ev, const uint8_t *frame, uint16_t len, uint8_t depth)
{
    if (ev == OD_RXQ_ARRIVED) {
        (void)pthread_mutex_lock(&g_arrival_lock);
        if (g_block_arrival) {
            g_arrival_entered = true;
            (void)pthread_cond_broadcast(&g_arrival_cond);
            while (!g_release_arrival) {
                (void)pthread_cond_wait(&g_arrival_cond, &g_arrival_lock);
            }
        }
        (void)pthread_mutex_unlock(&g_arrival_lock);
    }
    if (g_rep_n < REPORT_MAX) {
        g_rep[g_rep_n].ev = ev;
        g_rep[g_rep_n].len = len;
        g_rep[g_rep_n].depth = depth;
        g_rep[g_rep_n].first = (frame != NULL && len > 0u) ? frame[0] : 0xEEu;
        ++g_rep_n;
    }
}

/* ------------------------------------------------------------------------------ helpers --- */

static void reset_all(void)
{
    (void)od_rxq_reset();
    g_rep_n = 0u;
}

/* A frame whose first byte identifies it, so FIFO order is checkable by value. */
static bool push_marked(uint8_t mark, uint16_t len, uint32_t tag)
{
    uint8_t buf[OD_RXQ_FRAME_MAX];

    memset(buf, mark, len);
    buf[0] = mark;
    return od_rxq_push(buf, len, tag);
}

/* ----------------------------------------------------------------------------- the cases --- */

static void test_order(void)
{
    od_rxq_item_t *item;

    CASE("frames come back in arrival order, each carrying its own tag");
    reset_all();
    CHECK(push_marked(0xA1u, 8u, 111u));
    CHECK(push_marked(0xA2u, 9u, 222u));
    CHECK(push_marked(0xA3u, 10u, 333u));
    CHECK(od_rxq_depth() == 3u);
    CHECK(od_rxq_pending());

    item = od_rxq_peek();
    CHECK(item != NULL && item->data[0] == 0xA1u && item->len == 8u && item->tag == 111u);
    od_rxq_consume();
    item = od_rxq_peek();
    CHECK(item != NULL && item->data[0] == 0xA2u && item->len == 9u && item->tag == 222u);
    od_rxq_consume();
    item = od_rxq_peek();
    CHECK(item != NULL && item->data[0] == 0xA3u && item->len == 10u && item->tag == 333u);
    od_rxq_consume();

    CASE("and the ring is then empty");
    CHECK(od_rxq_peek() == NULL);
    CHECK(!od_rxq_pending());
    CHECK(od_rxq_depth() == 0u);

    CASE("peek is idempotent -- it does not consume");
    reset_all();
    CHECK(push_marked(0xB0u, 4u, 7u));
    CHECK(od_rxq_peek() == od_rxq_peek());
    CHECK(od_rxq_depth() == 1u);

    CASE("consume on an empty ring does nothing rather than handing out a live slot");
    reset_all();
    od_rxq_consume();
    od_rxq_consume();
    CHECK(od_rxq_depth() == 0u);
    CHECK(push_marked(0xB1u, 4u, 9u));
    CHECK(od_rxq_peek() != NULL && od_rxq_peek()->data[0] == 0xB1u);
}

static void test_the_slot_is_writable(void)
{
    od_rxq_item_t *item;

    /* The dispatcher decrypts IN PLACE into the peeked slot, so a const item would have forced a
     * 256-byte stack copy per frame -- up to a full window of them per pass. */
    CASE("the peeked slot is mutable, and the mutation survives to the next peek");
    reset_all();
    CHECK(push_marked(0xC0u, 16u, 1u));
    item = od_rxq_peek();
    CHECK(item != NULL);
    item->data[0] = 0xC9u;
    item->len = 12u;
    CHECK(od_rxq_peek()->data[0] == 0xC9u);
    CHECK(od_rxq_peek()->len == 12u);
}

static void test_full_ring_keeps_the_oldest(void)
{
    unsigned i;
    unsigned admitted = 0u;
    od_rxq_item_t *item;

    /* USABLE CAPACITY IS SLOTS - 1: one slot is always the gap that distinguishes full from empty.
     * Which frame loses matters more than how many fit. Overwriting the tail to admit the newest
     * would drop the frame the consumer is about to dispatch -- under a PIPE window that is the
     * frame whose ACK refunds a slot, so the transfer would STALL rather than lose a chunk. */
    CASE("the ring fills to SLOTS - 1 and then refuses");
    reset_all();
    for (i = 0u; i < OD_RXQ_SLOTS + 4u; ++i) {
        if (push_marked((uint8_t)(i + 1u), 4u, 5u)) {
            ++admitted;
        }
    }
    CHECK(admitted == (unsigned)(OD_RXQ_SLOTS - 1u));
    CHECK(od_rxq_depth() == (uint8_t)(OD_RXQ_SLOTS - 1u));

    CASE("and it is the NEWEST frame that is refused -- the head of the queue is untouched");
    item = od_rxq_peek();
    CHECK(item != NULL && item->data[0] == 1u);

    CASE("a full ring reports DROP_FULL, once per refused frame");
    {
        unsigned fulls = 0u;
        for (i = 0u; i < g_rep_n; ++i) {
            if (g_rep[i].ev == OD_RXQ_DROP_FULL) {
                ++fulls;
            }
        }
        CHECK(fulls == 5u);              /* SLOTS + 4 offered, SLOTS - 1 admitted */
    }

    CASE("consuming one frame frees exactly one slot");
    od_rxq_consume();
    CHECK(push_marked(0xFFu, 4u, 5u));
    CHECK(!push_marked(0xFEu, 4u, 5u));
}

static void test_admission(void)
{
    static uint8_t big[OD_RXQ_FRAME_MAX + 1u];

    CASE("an empty frame is refused and reported as EMPTY, not as a full ring");
    reset_all();
    memset(big, 0x5Au, sizeof big);
    CHECK(!od_rxq_push(big, 0u, 1u));
    CHECK(g_rep_n == 1u && g_rep[0].ev == OD_RXQ_DROP_EMPTY);

    CASE("a NULL frame is refused the same way rather than crashing");
    reset_all();
    CHECK(!od_rxq_push(NULL, 8u, 1u));
    CHECK(g_rep_n == 1u && g_rep[0].ev == OD_RXQ_DROP_EMPTY);

    CASE("a frame above the admission bound is TOO_LARGE, distinctly from a full ring");
    reset_all();
    CHECK(!od_rxq_push(big, (uint16_t)(OD_RXQ_VALUE_MAX_BLE + 1u), 1u));
    CHECK(g_rep_n == 1u && g_rep[0].ev == OD_RXQ_DROP_TOO_LARGE);
    CHECK(g_rep[0].len == (uint16_t)(OD_RXQ_VALUE_MAX_BLE + 1u));

    /* The three reasons being distinguishable is the whole point of the seam: Nordic reported all
     * of them as "pipe queue full", so a malformed frame looked like backpressure. */
    CASE("253 value bytes are admitted while the storage slot remains 256 bytes wide");
    reset_all();
    CHECK(OD_RXQ_FRAME_MAX == 256u);
    CHECK(OD_RXQ_VALUE_MAX_BLE == 253u);
    CHECK(od_rxq_push(big, (uint16_t)OD_RXQ_VALUE_MAX_BLE, 1u));
    CHECK(od_rxq_peek() != NULL && od_rxq_peek()->len == (uint16_t)OD_RXQ_VALUE_MAX_BLE);
    CHECK(g_rep_n == 1u && g_rep[0].ev == OD_RXQ_ARRIVED);
}

static void test_stale_tags_are_discarded(void)
{
    od_rxq_item_t *item;

    CASE("stale heads are consumed without skipping the first live-tag frame");
    reset_all();
    CHECK(push_marked(0x31u, 4u, 31u));
    CHECK(push_marked(0x32u, 4u, 32u));
    CHECK(push_marked(0x77u, 4u, 77u));
    CHECK(push_marked(0x33u, 4u, 33u));
    CHECK(od_rxq_discard_stale(77u) == 2u);
    item = od_rxq_peek();
    CHECK(item != NULL && item->tag == 77u && item->data[0] == 0x77u);

    CASE("a live head is never consumed by stale discard");
    CHECK(od_rxq_discard_stale(77u) == 0u);
    CHECK(od_rxq_peek() == item);
    od_rxq_consume();

    CASE("stale discard drains the remainder when no live frame follows");
    CHECK(od_rxq_discard_stale(77u) == 1u);
    CHECK(od_rxq_peek() == NULL);
}

static void test_report_depth_is_pre_push(void)
{
    CASE("depth is reported PRE-push, so a frame never reports its own arrival as backlog");
    reset_all();
    CHECK(push_marked(0xD0u, 4u, 1u));
    CHECK(g_rep_n == 1u && g_rep[0].ev == OD_RXQ_ARRIVED && g_rep[0].depth == 0u);
    CHECK(push_marked(0xD1u, 4u, 1u));
    CHECK(g_rep_n == 2u && g_rep[1].depth == 1u);
    CHECK(push_marked(0xD2u, 4u, 1u));
    CHECK(g_rep_n == 3u && g_rep[2].depth == 2u);

    CASE("and the report sees the arriving bytes, not the stored copy");
    CHECK(g_rep[0].first == 0xD0u);
    CHECK(g_rep[2].first == 0xD2u);
}

static void test_reset(void)
{
    unsigned i;

    CASE("reset discards every unconsumed frame and says how many");
    reset_all();
    for (i = 0u; i < 5u; ++i) {
        CHECK(push_marked((uint8_t)(i + 1u), 4u, 1u));
    }
    CHECK(od_rxq_reset() == 5u);
    CHECK(od_rxq_depth() == 0u);
    CHECK(od_rxq_peek() == NULL);

    CASE("reset on an empty ring is 0 and safe");
    CHECK(od_rxq_reset() == 0u);
    CHECK(od_rxq_reset() == 0u);

    CASE("a partially drained ring resets only what is left");
    reset_all();
    for (i = 0u; i < 6u; ++i) {
        CHECK(push_marked((uint8_t)(i + 1u), 4u, 1u));
    }
    od_rxq_consume();
    od_rxq_consume();
    CHECK(od_rxq_reset() == 4u);

    CASE("and the ring is usable immediately afterwards");
    CHECK(push_marked(0x77u, 4u, 1u));
    CHECK(od_rxq_peek() != NULL && od_rxq_peek()->data[0] == 0x77u);

    CASE("reset does not report -- it is consumer-side bookkeeping, not a frame event");
    reset_all();
    CHECK(push_marked(0x01u, 4u, 1u));
    g_rep_n = 0u;
    (void)od_rxq_reset();
    CHECK(g_rep_n == 0u);
}

struct race_push {
    uint8_t data[8];
    bool pushed;
};

static void *race_push_main(void *opaque)
{
    struct race_push *push = (struct race_push *)opaque;

    push->pushed = od_rxq_push(push->data, (uint16_t)sizeof(push->data), 0xC9u);
    return NULL;
}

static void test_reset_racing_a_producer(void)
{
    pthread_t producer;
    struct race_push push;
    unsigned i;
    int rc;

    CASE("a push paused before publication survives a concurrent consumer reset");
    reset_all();
    /* Move both indices away from zero so a broken reset-both-indices implementation cannot pass
     * merely because its destructive reset happens to write their current values. */
    for (i = 0u; i < 5u; ++i) {
        CHECK(push_marked((uint8_t)(0x40u + i), 4u, 1u));
        od_rxq_consume();
    }
    for (i = 0u; i < 3u; ++i) {
        CHECK(push_marked((uint8_t)(0x50u + i), 4u, 2u));
    }

    memset(&push, 0, sizeof(push));
    memset(push.data, 0xC9, sizeof(push.data));
    (void)pthread_mutex_lock(&g_arrival_lock);
    g_block_arrival = true;
    g_arrival_entered = false;
    g_release_arrival = false;
    (void)pthread_mutex_unlock(&g_arrival_lock);

    rc = pthread_create(&producer, NULL, race_push_main, &push);
    CHECK(rc == 0);
    if (rc != 0) {
        (void)pthread_mutex_lock(&g_arrival_lock);
        g_block_arrival = false;
        (void)pthread_mutex_unlock(&g_arrival_lock);
        return;
    }

    (void)pthread_mutex_lock(&g_arrival_lock);
    while (!g_arrival_entered) {
        (void)pthread_cond_wait(&g_arrival_cond, &g_arrival_lock);
    }
    (void)pthread_mutex_unlock(&g_arrival_lock);

    /* The three old frames were published; the producer is paused before publishing the fourth. */
    CHECK(od_rxq_reset() == 3u);

    (void)pthread_mutex_lock(&g_arrival_lock);
    g_release_arrival = true;
    (void)pthread_cond_broadcast(&g_arrival_cond);
    (void)pthread_mutex_unlock(&g_arrival_lock);
    CHECK(pthread_join(producer, NULL) == 0);

    (void)pthread_mutex_lock(&g_arrival_lock);
    g_block_arrival = false;
    (void)pthread_mutex_unlock(&g_arrival_lock);

    CHECK(push.pushed);
    CHECK(od_rxq_depth() == 1u);
    CHECK(od_rxq_peek() != NULL && od_rxq_peek()->tag == 0xC9u);
    CHECK(od_rxq_peek() != NULL && od_rxq_peek()->data[0] == 0xC9u);
}

static void test_wraparound(void)
{
    unsigned round;
    unsigned i;

    /* The index arithmetic is mod SLOTS on uint8_t. Pushing many times more than the ring holds is
     * how an off-by-one in the wrap shows up as a frame delivered twice or skipped. */
    CASE("the ring survives many wraps with FIFO intact");
    reset_all();
    for (round = 0u; round < 40u; ++round) {
        for (i = 0u; i < 7u; ++i) {
            CHECK(push_marked((uint8_t)(i + 1u), (uint16_t)(4u + i), (uint32_t)(round * 10u + i)));
        }
        for (i = 0u; i < 7u; ++i) {
            od_rxq_item_t *item = od_rxq_peek();
            CHECK(item != NULL);
            if (item != NULL) {
                CHECK(item->data[0] == (uint8_t)(i + 1u));
                CHECK(item->len == (uint16_t)(4u + i));
                CHECK(item->tag == (uint32_t)(round * 10u + i));
            }
            od_rxq_consume();
        }
        CHECK(od_rxq_depth() == 0u);
    }
}

int main(void)
{
    test_order();
    test_the_slot_is_writable();
    test_full_ring_keeps_the_oldest();
    test_admission();
    test_stale_tags_are_discarded();
    test_report_depth_is_pre_push();
    test_reset();
    test_reset_racing_a_producer();
    test_wraparound();

    printf("rxq: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails == 0u ? 0 : 1;
}
