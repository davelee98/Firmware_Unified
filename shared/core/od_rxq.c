/* od_rxq.c -- see od_rxq.h; the SPSC contract there is the specification. */

#include "od_rxq.h"

#include "od_log.h"

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
#include "od_rxq_app.h"
#endif

#include <stdio.h>
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

/* The per-frame arrival line, DEBUG only.
 *
 * Debug-gated rather than merely level-filtered: at INFO the whole body preprocesses away, so a
 * normal build spends nothing on the 192-byte record, the hex formatting or the two seam calls.
 * The runtime level test inside od_log_debug() would have skipped only the emit.
 *
 * Depth is the PRE-push count, matching the TX line's pre-enqueue depth, so a healthy path reads
 * [BLE][Q:0] and a rising Q means arrivals are outrunning the drain. */
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
static void log_arrival(const uint8_t *data, uint16_t len, uint8_t depth)
{
    uint16_t cmd;
    bool encrypted;
    char label[48];
    char line[192];

    cmd = (len >= 2u) ? (uint16_t)(((uint16_t)data[0] << 8) | data[1]) : data[0];
    if (od_rxq_app_quiet(cmd)) {
        return;
    }
    /* ERX / URX: does this frame carry the app-layer CCM envelope? Mirrors the dispatcher's gate
     * -- the two handshake opcodes are answered before it, and a frame too short to hold
     * nonce+tag cannot be wrapped. ORIGIN_LAN_TLS is omitted deliberately: this ring is BLE only,
     * LAN frames never reach it. So the token reports the frame's FORM, not its intent; anything
     * URX while encryption is on is rejected by the dispatcher. */
    encrypted = od_rxq_app_encryption_enabled() &&
                cmd != CMD_AUTHENTICATE && cmd != CMD_FIRMWARE_VERSION &&
                len >= BLE_CMD_HEADER_SIZE + ENCRYPTION_NONCE_SIZE + ENCRYPTION_TAG_SIZE;

    (void)snprintf(label, sizeof(label), "[BLE][Q:%u] %s 0x%04X (%u B): ",
                   (unsigned)depth, encrypted ? "ERX" : "URX", cmd, (unsigned)len);
    od_log_hex_line(line, sizeof(line), label, data, len);
    od_log_debug("%s", line);
}
#else
static void log_arrival(const uint8_t *data, uint16_t len, uint8_t depth)
{
    (void)data;
    (void)len;
    (void)depth;
}
#endif

bool od_rxq_push(const uint8_t *data, uint16_t len, uint32_t tag)
{
    uint8_t head;
    uint8_t tail;
    uint8_t next;

    if (data == NULL || len == 0u) {
        od_log_warn("Empty BLE frame received, dropping");
        return false;
    }
    if (len > OD_RXQ_VALUE_MAX_BLE) {
        od_log_warn("Command too large for queue (%u > %u), dropping",
                    (unsigned)len, (unsigned)OD_RXQ_VALUE_MAX_BLE);
        return false;
    }

    head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);   /* we are the only writer of head */
    tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    next = (uint8_t)((head + 1u) % OD_RXQ_SLOTS);
    if (next == tail) {
        /* THE OLDEST FRAMES WIN. Overwriting the tail to admit this one would drop a frame the
         * consumer is about to dispatch, and under a PIPE window that is the frame whose ACK
         * refunds the slot -- so the transfer would stall rather than merely lose a chunk. */
        /* error, not warn: a full ring is resource exhaustion, not one malformed frame. */
        od_log_error("Command queue full, dropping command (%u slots)",
                     (unsigned)OD_RXQ_SLOTS);
        return false;
    }

    /* Logged BEFORE the payload is committed, while `depth` still describes what the frame
     * arrived into: a rising depth is the diagnostic, and reading it after the push would report
     * the frame's own arrival as backlog. */
    log_arrival(data, len, occupancy(head, tail));

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

uint8_t od_rxq_discard_stale(od_rxq_tag_is_live_fn is_live, void *context)
{
    uint8_t dropped = 0u;
    od_rxq_item_t *item;

    if (is_live == NULL) {
        return 0u;
    }
    while ((item = od_rxq_peek()) != NULL && !is_live(item->tag, context)) {
        od_rxq_consume();
        ++dropped;
    }
    return dropped;
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
