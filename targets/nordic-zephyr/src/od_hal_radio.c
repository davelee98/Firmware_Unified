/* od_hal_radio.c -- this target's implementation of shared/hal/od_hal_radio.h.
 *
 * One notify, and one liveness question. Everything about WHEN to send, in what order, and what to
 * do about a refusal belongs to od_txq; this file only knows how to hand bytes to the BT host.
 *
 * THE 200 ms INLINE RETRY IS GONE, and its removal is the point of binding this HAL rather than a
 * detail of it. The sender this replaces looped up to 200 times with a 1 ms sleep when
 * bt_gatt_notify() returned -ENOMEM, because a multi-chunk config read could outrun the TX pool
 * and there was nowhere to put a frame that could not go now. That loop ran on the MAIN THREAD --
 * the same thread that drives the display and feeds the watchdog -- so transport backpressure
 * became a main-thread stall of up to 200 ms per frame, and a config read could spend seconds
 * there. od_txq is the somewhere: RETRY leaves the frame at the head of the queue and the next
 * pass offers it again, so a busy pool costs a pass instead of a thread.
 *
 * That is also why RETRY must never be reported as ERROR. -ENOMEM is momentary pool exhaustion,
 * which under a PIPE upload is routine rather than exceptional; mapping it to ERROR would drop a
 * response every time the buffers filled.
 */

#include "od_hal_radio.h"

#include "opendisplay_ble.h"
#include "opendisplay_pipe.h"

#include "od_txq.h"

#include <stddef.h>

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag, const uint8_t *frame,
                                    uint16_t len)
{
  if (frame == NULL || len == 0u) {
    return OD_RADIO_ERROR;
  }
  if (origin != OD_ORIGIN_BLE) {
    /* This target has no LAN transport, so a LAN-origin frame is a routing bug rather than a
     * busy link. Never retried: retrying a permanent error turns the drain into a spin. */
    return OD_RADIO_ERROR;
  }
  if (!od_hal_radio_tag_is_live(origin, tag)) {
    return OD_RADIO_GONE;
  }
  if (!opendisplay_ble_pipe_notify_enabled()) {
    /* Connected but not yet subscribed, or mid-teardown. Retryable: the CCCD write may still
     * arrive, and dropping here would lose the first response of every session. */
    return OD_RADIO_RETRY;
  }
  return opendisplay_ble_pipe_notify(frame, len) ? OD_RADIO_SENT : OD_RADIO_RETRY;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
  if (origin != OD_ORIGIN_BLE) {
    return false;
  }
  /* The connection GENERATION, not a handle: this target counts connections rather than packing an
   * owner word, but the property od_txq needs is the same -- a frame queued by a departed
   * connection must never be delivered to whoever came after it. */
  return tag == od_pipe_conn_gen();
}

/* IMPLEMENTED FOR shared/core/od_txq.h, and empty. od_txq.c logs the discard itself, keeping the
 * INFO level this target chose -- on a normal disconnect mid-upload it fires once per queued
 * frame -- with ESP32's origin/tag content. The connection generation this used to print is
 * od_pipe_conn_gen(), which the shared line has no way to reach; the reply's own tag is what
 * distinguishes a departed peer from an identity bug, and that is in the line. */
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
  (void)rp;
  (void)len;
  (void)why;
}
