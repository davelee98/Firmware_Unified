/* od_rxq.h -- the BLE receive ring, once.
 *
 * SPSC. Push runs on the stack's callback task (NimBLE's onWrite, Zephyr's bt_gatt write handler);
 * peek/consume/reset run on the loop or main thread. The producer publishes the head with RELEASE
 * after the payload lands, so the consumer never observes a slot before its bytes are visible.
 * That is the whole synchronisation contract -- there is no lock, and adding one would put a
 * stack callback behind a mutex the loop holds across a panel refresh.
 *
 * PEEK/CONSUME, NOT A COPYING POP. The consumer is the only party that may touch a slot until it
 * advances the tail, so handing out a pointer is safe and avoids a 256-byte stack buffer plus one
 * memcpy per frame -- up to a full PIPE window of them per pass. The peeked slot is deliberately
 * MUTABLE: the dispatcher decrypts in place.
 *
 * EVERY SLOT CARRIES ITS WRITER'S IDENTITY (`tag`), and that is what makes stale frames
 * self-discarding. BLE connection handles are reused, so a frame queued by a departed instance is
 * indistinguishable from the new owner's by transport alone; the dispatcher executes a frame only
 * if its tag still equals the live owner word. This replaced an RX-boundary scheme that captured a
 * head at link-down: that boundary lived in the departing instance's slot and was lost whenever
 * the stack reissued the handle before the loop scanned it.
 *
 * THIS MODULE DOES NOT LOG. od_log.h is target-local and shared/ may not include it, so arrivals
 * and drops are reported through od_rxq_app_report() -- one site for both targets. A copy of that
 * logic in each transport callback is exactly how the two targets drifted before: one reported a
 * malformed frame as "queue full".
 */

#ifndef OD_RXQ_H
#define OD_RXQ_H

#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"   /* OD_STATIC_ASSERT -- C99-safe, unlike raw _Static_assert */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keep storage width and wire admission separate, exactly as od_txq does. OD_BLE_MAX_FRAME is the
 * complete ATT MTU -- opcode(1) + handle(2) + value -- so a write carries at most 253 value bytes.
 * The slot stays 256 bytes wide; that storage headroom is not permission to admit a 256-byte
 * value. The dispatcher refuses above 244, leaving 245..253 as the observable protocol-NACK band. */
#define OD_RXQ_FRAME_MAX      OD_BLE_MAX_FRAME
#define OD_RXQ_VALUE_MAX_BLE (OD_BLE_MAX_FRAME - 3u)

OD_STATIC_ASSERT(OD_RXQ_VALUE_MAX_BLE <= OD_RXQ_FRAME_MAX,
                 "BLE value admission must fit the RX storage slot");

/* Depth. The target sets this from its own PIPE window -- PIPE_MAX_W + 2, so usable capacity
 * (SLOTS - 1) covers a full window plus its END -- and asserts the relationship where both are
 * visible. shared/ cannot see PIPE_MAX_W, so the default here is the widest window any target
 * runs; a target with a narrower one narrows this with it rather than carrying slots it can never
 * fill. Setting it BELOW the derived value caps the effective window and costs throughput: a
 * deliberate trade, never a link-time discovery. */
#ifndef OD_RXQ_SLOTS
#define OD_RXQ_SLOTS 34u
#endif

typedef struct {
    uint8_t  data[OD_RXQ_FRAME_MAX];
    uint16_t len;
    /* Packed identity word of the instance that wrote this frame, stamped in the write callback.
     * Written BEFORE the release-store that publishes the head, exactly like data and len, or the
     * consumer's acquire load would not be guaranteed to see it -- and a frame would dispatch
     * against the wrong identity. */
    uint32_t tag;
} od_rxq_item_t;

/* What happened to one inbound frame. Reported rather than logged; the target decides the wording,
 * whether to suppress mid-stream image data, and how to spell the encrypted/plaintext token. */
typedef enum {
    OD_RXQ_ARRIVED = 0,     /* queued; `depth` is the pre-push occupancy */
    OD_RXQ_DROP_EMPTY,      /* len == 0 or a NULL pointer */
    OD_RXQ_DROP_TOO_LARGE,  /* len > OD_RXQ_VALUE_MAX_BLE -- above transport admission */
    OD_RXQ_DROP_FULL        /* the ring is full; the OLDEST frames are kept, this one is refused */
} od_rxq_event_t;

/* IMPLEMENTED BY THE TARGET. Called from the STACK CALLBACK TASK, so it must not block, must not
 * take a lock the loop holds, and must not dispatch. `frame`/`len` are the arriving bytes, valid
 * only for the duration of the call. */
void od_rxq_app_report(od_rxq_event_t ev, const uint8_t *frame, uint16_t len, uint8_t depth);

/* Producer side, stack callback task only. False means dropped, and the reason has been reported. */
bool od_rxq_push(const uint8_t *data, uint16_t len, uint32_t tag);

/* Consumer side, loop/main thread only. */
od_rxq_item_t *od_rxq_peek(void);     /* NULL = empty; the slot stays valid until consume */
void           od_rxq_consume(void);  /* advance past the peeked slot */
typedef bool (*od_rxq_tag_is_live_fn)(uint32_t tag, void *context);
/* Consume consecutive heads whose writer is no longer live, stopping before the first live frame.
 * `is_live` is called AFTER each head is peeked and again for every candidate: connection identity
 * can change concurrently, so accepting one by-value "current tag" for the whole drain would drop
 * a new owner's frames or preserve an old owner's. NULL is conservative and discards nothing. */
uint8_t        od_rxq_discard_stale(od_rxq_tag_is_live_fn is_live, void *context);
uint8_t        od_rxq_head(void);     /* producer-side index, for an activity poll */
uint8_t        od_rxq_depth(void);    /* unconsumed frames */
bool           od_rxq_pending(void);

/* Discard every unconsumed frame; returns how many.
 *
 * SPSC-SAFE BY CONSTRUCTION, and the contract is not optional: this snapshots the producer's head
 * with ACQUIRE and stores that snapshot into the tail with RELEASE. It writes NEITHER the head NOR
 * any slot payload. A conventional "reset both indices and clear the slots" would race a producer
 * mid-copy, because the push writes its payload before publishing the head. A push in flight
 * either published before the snapshot (discarded here) or after it (survives, carrying the
 * departing owner's tag, and is dropped at dispatch) -- which is why this needs no retry and no
 * second pass.
 *
 * MUST NOT RUN WHILE A PEEK IS OUTSTANDING: the consumer holds a pointer into the tail slot across
 * dispatch. */
uint8_t od_rxq_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_RXQ_H */
