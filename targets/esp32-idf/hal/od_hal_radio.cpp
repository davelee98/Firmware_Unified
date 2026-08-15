/* od_hal_radio.cpp -- ESP32's implementation of shared/hal/od_hal_radio.h.
 *
 * One notify, and one liveness question. Everything about WHEN to send, in what order, and what to
 * do about a refusal belongs to od_txq; this file only knows how to hand bytes to NimBLE.
 */

#include "od_hal_radio.h"

#include "ble_transport.h"
#include "link_owner.h"
#ifdef OPENDISPLAY_HAS_WIFI
#include "wifi_service.h"
#endif

extern "C" od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                               const uint8_t *frame, uint16_t len)
{
    if (frame == nullptr || len == 0u) {
        return OD_RADIO_ERROR;
    }
    if (origin != OD_ORIGIN_BLE) {
#ifdef OPENDISPLAY_HAS_WIFI
        /* LAN responses DO come through here: od_reply() routes a TLS-LAN frame to the queue
         * plain (SECTION 9 rule 4 -- TLS already protects it), so the queue must be able to
         * deliver it. Dropping it as a routing error, which an earlier version of this file did,
         * loses every TLS client's response silently: the send succeeds from the core's point of
         * view and nothing ever reaches the socket.
         *
         * opendisplay_lan_send_frame() is void and does its own buffering, so there is no
         * backpressure to report -- SENT is the only honest answer available. */
        opendisplay_lan_send_frame(frame, len);
        return OD_RADIO_SENT;
#else
        /* No LAN transport compiled in, so a LAN-origin frame is genuinely a routing bug. Never
         * retried: retrying a permanent error turns the drain into a spin. */
        return OD_RADIO_ERROR;
#endif
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
#ifdef OPENDISPLAY_HAS_WIFI
        /* The LAN transport owns its own connection lifetime and a queued LAN frame is delivered
         * synchronously, so there is no stale-instance window for the queue to guard against.
         * Answering false here would make od_txq_commit() discard every TLS response as GONE. */
        (void)tag;
        return true;
#else
        return false;
#endif
    }
    /* An instance identity, not a connection handle: handles are reused, so a frame queued by a
     * dead instance must never be delivered to whoever inherited its slot. */
    return linkIsOwnerWord(tag);
}
