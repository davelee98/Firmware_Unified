/* od_rxq.c -- see od_rxq.h; the SPSC contract there is the specification. */

#include "od_rxq.h"

#include <string.h>

/* One ring, never one per connection. Per-frame identity (`tag`) is what distinguishes instances,
 * so nothing that holds frames is replicated per link. */
static od_rxq_item_t s_rx[OD_RXQ_SLOTS];
static volatile uint8_t s_head;
static volatile uint8_t s_tail;

OD_STATIC_ASSERT(OD_RXQ_SLOTS >= 3u, "a ring of two has one usable slot");
OD_STATIC_ASSERT(OD_RXQ_SLOTS <= 255u, "head and tail are uint8_t");

static uint8_t occupancy(uint8_t head, uint8_t tail)
{
    return (uint8_t)((head - tail + OD_RXQ_SLOTS) % OD_RXQ_SLOTS);
}

bool od_rxq_push(const uint8_t *data, uint16_t len, uint32_t tag)
{
    uint8_t head;
    uint8_t tail;
    uint8_t next;

    if (data == NULL || len == 0u) {
        od_rxq_app_report(OD_RXQ_DROP_EMPTY, data, len, 0u);
        return false;
    }
    if (len > OD_RXQ_FRAME_MAX) {
        od_rxq_app_report(OD_RXQ_DROP_TOO_LARGE, data, len, 0u);
        return false;
    }

    head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);   /* we are the only writer of head */
    tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    next = (uint8_t)((head + 1u) % OD_RXQ_SLOTS);
    if (next == tail) {
        /* THE OLDEST FRAMES WIN. Overwriting the tail to admit this one would drop a frame the
         * consumer is about to dispatch, and under a PIPE window that is the frame whose ACK
         * refunds the slot -- so the transfer would stall rather than merely lose a chunk. */
        od_rxq_app_report(OD_RXQ_DROP_FULL, data, len, occupancy(head, tail));
        return false;
    }

    /* Reported BEFORE the payload is committed, while `depth` still describes what the frame
     * arrived into: a rising depth is the diagnostic, and reading it after the push would report
     * the frame's own arrival as backlog. */
    od_rxq_app_report(OD_RXQ_ARRIVED, data, len, occupancy(head, tail));

    memcpy(s_rx[head].data, data, len);
    s_rx[head].len = len;
    s_rx[head].tag = tag;
    /* RELEASE, and everything above must be complete before it: this store is what publishes the
     * slot, and the consumer's ACQUIRE load of the head is what makes all of it visible. */
    __atomic_store_n(&s_head, next, __ATOMIC_RELEASE);
    return true;
}

od_rxq_item_t *od_rxq_peek(void)
{
    const uint8_t tail = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
    const uint8_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);

    if (tail == head) {
        return NULL;
    }
    return &s_rx[tail];
}

void od_rxq_consume(void)
{
    const uint8_t tail = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);

    if (tail == __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
        return;                       /* nothing peeked; advancing would hand out a live slot */
    }
    __atomic_store_n(&s_tail, (uint8_t)((tail + 1u) % OD_RXQ_SLOTS), __ATOMIC_RELEASE);
}

uint8_t od_rxq_head(void)
{
    return __atomic_load_n(&s_head, __ATOMIC_RELAXED);
}

uint8_t od_rxq_depth(void)
{
    /* RELAXED both: a snapshot for a log line or an activity poll, not a synchronisation point.
     * The producer may push concurrently, in which case this reads one frame stale -- the correct
     * answer to "how deep was it a moment ago". */
    const uint8_t head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    const uint8_t tail = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);

    return occupancy(head, tail);
}

bool od_rxq_pending(void)
{
    return __atomic_load_n(&s_tail, __ATOMIC_RELAXED) !=
           __atomic_load_n(&s_head, __ATOMIC_RELAXED);
}

uint8_t od_rxq_reset(void)
{
    const uint8_t tail = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
    const uint8_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    uint8_t dropped;

    if (tail == head) {
        return 0u;
    }
    dropped = occupancy(head, tail);
    /* Advance the tail and touch NOTHING else. Clearing each discarded slot writes producer
     * territory -- the producer owns every slot from `head` onward, and a slot this would walk can
     * already have been handed to a concurrent push. */
    __atomic_store_n(&s_tail, head, __ATOMIC_RELEASE);
    return dropped;
}
