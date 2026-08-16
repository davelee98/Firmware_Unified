/* od_config_read.h -- CONFIG_READ as a resumable producer.
 *
 * WHY IT CANNOT STAY SYNCHRONOUS. The shipped handler emits every chunk in one call and drains the
 * TX ring between them, which works only because it can block the loop task it is running on. With
 * finite egress backpressure that stops being true: once a flush deadline leaves the queue full,
 * the handler has nowhere to put the next chunk and no way to report it -- the failure frame it
 * would send needs a slot too. So the read becomes a producer that emits one chunk per pass and
 * simply stays pending when capacity is unavailable. It never truncates, never spins, and never
 * needs a frame to say "not yet".
 *
 * WHY IT MUST EXCLUDE CONFIG WRITES WHILE ACTIVE. The producer reads from the target's config
 * scratch, and CONFIG_WRITE -> reload -> parse overwrites that same buffer. A write landing between
 * two chunks would splice two different configs into one read-back that still passes CRC. So while
 * a read is active, any config-mutating command and any second CONFIG_READ must be DEFERRED by the
 * dispatcher BEFORE decrypt and before reservation -- deferring after decrypt would advance the
 * replay window for a frame that is then re-dispatched.
 *
 * Cancelling a partly-sent read is also not free: the host was told a chunk count in chunk 0 and
 * would wait forever for the rest. Only od_core_reset() or the death of the tag cancels one.
 *
 * CONTEXT: single loop/main task. No locks.
 */

#ifndef OD_CONFIG_READ_H
#define OD_CONFIG_READ_H

#include "od_txq.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Begin a read. `r` is the dispatcher's one-frame reservation, TRANSFERRED to the producer: it
 * pays for chunk 0, or for the read error if the blob could not be loaded. The caller must not
 * release or reuse it afterwards.
 *
 * `blob` must stay valid and unmodified until the producer completes -- that is what the
 * dispatcher's DEFERRED rule guarantees. A NULL blob means "load failed" and emits the 4-byte
 * error frame, which is a completed read, not a pending one.
 *
 * Returns the od_reply status of the first frame. Starting a read while one is active is
 * OD_TXQ_INVARIANT: the dispatcher is supposed to have deferred it. */
od_txq_status_t od_config_read_start(const od_reply_t *rp, od_tx_reservation_t *r,
                                     const uint8_t *blob, uint32_t blob_len);

/* One pass. Reserves a slot and emits the next chunk if it can; OD_TXQ_FULL simply leaves the
 * producer pending for the next pass, which is the whole point and is not an error. Returns
 * OD_TXQ_OK and goes inactive when the last chunk is queued. Safe to call when idle. */
od_txq_status_t od_config_read_pump(void);

/* Is a read in flight? The dispatcher's DEFERRED gate reads this. */
bool od_config_read_active(void);

/* Abandon a read, releasing any unused reservation. For od_core_reset() and for the death of the
 * tag being read to -- NOT for ordinary backpressure, which is what pending means. Queued chunks
 * for a dead tag are discarded by od_txq's own GONE path. */
void od_config_read_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CONFIG_READ_H */
