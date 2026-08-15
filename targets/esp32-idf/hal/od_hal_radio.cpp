/* od_hal_radio.cpp -- ESP32's implementation of shared/hal/od_hal_radio.h.
 *
 * One notify, and one liveness question. Everything about WHEN to send, in what order, and what to
 * do about a refusal belongs to od_txq; this file only knows how to hand bytes to NimBLE.
 */

#include "od_hal_radio.h"

#include "ble_transport.h"
#include "link_owner.h"

extern "C" od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                               const uint8_t *frame, uint16_t len)
{
    if (frame == nullptr || len == 0u) {
        return OD_RADIO_ERROR;
    }
    if (origin != OD_ORIGIN_BLE) {
        /* LAN frames leave through the TCP transport, which the dispatcher calls directly -- it
         * dispatches synchronously from the loop and never shares this queue. Reaching here with a
         * LAN origin is a routing bug, not backpressure, so it must not be retried. */
        return OD_RADIO_ERROR;
    }
    if (!linkIsOwnerWord(tag)) {
        return OD_RADIO_GONE;
    }
    if (!ble.notifyReady()) {
        /* Connected but not yet subscribed, or mid-teardown. Retryable: the CCCD write may still
         * arrive, and dropping here would lose the first response of every session. */
        return OD_RADIO_RETRY;
    }
    /* BleTransport::notify() returns false for BACKPRESSURE specifically -- its own comment says
     * "retry next pass", not a hard failure -- so it maps to RETRY, never ERROR. Mapping it to
     * ERROR would drop a response every time the stack's TX buffers filled, which under a PIPE
     * upload is routine rather than exceptional. */
    return ble.notify(frame, len) ? OD_RADIO_SENT : OD_RADIO_RETRY;
}

extern "C" bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    if (origin != OD_ORIGIN_BLE) {
        return false;
    }
    /* An instance identity, not a connection handle: handles are reused, so a frame queued by a
     * dead instance must never be delivered to whoever inherited its slot. */
    return linkIsOwnerWord(tag);
}
