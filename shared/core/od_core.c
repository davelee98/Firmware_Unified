/* od_core.c -- see od_core.h. Three calls, and the ORDER of the first two matters.
 *
 * The producer is cancelled BEFORE the queue is dropped. Reversed, a producer still holding its
 * reservation can commit a chunk into the freshly emptied ring between the two calls, and that
 * chunk belongs to a read the caller has just decided is over -- it would go out to whoever
 * inherits the link, as chunk N of a transfer they never started.
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
