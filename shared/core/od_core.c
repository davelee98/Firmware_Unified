/* od_core.c -- see od_core.h.
 *
 * Three calls, written in dependency order: the producer that owns a reservation, then the queue
 * that reservation is against, then the session those frames would have been sealed with.
 *
 * THE ORDER IS NOT LOAD-BEARING, and saying so is worth more than a story about why it is. This
 * runs on the consumer context with nothing interleaved, and od_txq_reset() invalidates
 * outstanding tokens by generation anyway, so a producer cancelled second could not have committed
 * in between. What IS load-bearing is that all three happen -- which is the whole reason this
 * exists rather than three calls copied into each target's teardown.
 */

#include "od_core.h"

#include "od_config_read.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_txq.h"

void od_core_reset(void)
{
    od_config_read_cancel();
    od_txq_reset();
    od_session_clear(od_session_app_state());
}
