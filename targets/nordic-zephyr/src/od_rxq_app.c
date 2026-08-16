/* od_rxq_app.c -- this target's implementation of shared/core/od_rxq.h's reporting seam.
 *
 * One site for arrivals and for all three drop reasons. Nordic's previous intake logged only a
 * full queue, so an empty or oversized frame was reported as "pipe queue full" -- pointing at ring
 * depth for what was actually a malformed frame. That is the drift this seam exists to end.
 *
 * CALLED ON THE BT RX THREAD. od_log is non-blocking there, and the arrival line is the only place
 * a frame's delivery time (rather than its dispatch time) is visible.
 */

#include "od_rxq.h"

#include "od_log.h"

void od_rxq_app_report(od_rxq_event_t ev, const uint8_t *frame, uint16_t len, uint8_t depth)
{
  switch (ev) {
  case OD_RXQ_DROP_EMPTY:
    od_log_info("rx dropped: empty frame");
    return;
  case OD_RXQ_DROP_TOO_LARGE:
    /* Unreachable from the GATT layer, which now refuses over-length writes with ATT 0x0D so the
     * host learns of them. Kept because this seam is also the contract for any future producer. */
    od_log_info("rx dropped: %u B exceeds the %u B admission", (unsigned)len,
                (unsigned)OD_RXQ_FRAME_MAX);
    return;
  case OD_RXQ_DROP_FULL:
    od_log_info("rx dropped: ring full at %u slots", (unsigned)OD_RXQ_SLOTS);
    return;
  case OD_RXQ_ARRIVED:
    break;
  }

  /* The per-frame image-data opcodes are the bulk of a transfer and would drown the log; the
   * transfer's own progress reporting covers them. Everything else is one line with its
   * pre-push depth, so a rising queue is visible as arrivals outrunning the main thread. */
  if (frame != NULL && len >= 2u) {
    const uint16_t cmd = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
    if (cmd != CMD_DIRECT_WRITE_DATA && cmd != CMD_PIPE_WRITE_DATA) {
      od_log_info("rx cmd=0x%04X len=%u q=%u", (unsigned)cmd, (unsigned)len, (unsigned)depth);
    }
  }
}
