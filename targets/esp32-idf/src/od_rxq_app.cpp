/* od_rxq_app.cpp -- this target's implementation of shared/core/od_rxq.h's reporting seam.
 *
 * THE SINGLE RX LOG LINE, and the single place the three push failures are named. It lives behind
 * one seam, not in either transport callback, because a copy in each is how the two targets
 * drifted before: ESP32 had a hex line and separate "too large" / "empty" warnings, Nordic had
 * neither and reported all three failures as "queue full" -- pointing at ring depth for what was
 * actually a malformed frame.
 *
 * CALLED ON THE STACK CALLBACK TASK, so the timestamp is when the radio delivered the frame rather
 * than when the loop got to it. That ordering is the point of the line and cannot be had from the
 * consumer side. The cost is formatting plus a non-blocking write attempt: od_log gives any task
 * other than loop() a zero wait budget and discards the record rather than waiting above loop()'s
 * priority, so a frame's line can be dropped under burst. It compiles out entirely at the default
 * INFO level.
 */

#include "od_rxq.h"

#include "encryption.h"   // isEncryptionEnabled(), for the ERX/URX token
#include "od_log.h"

#include <stdio.h>

// Defined in display_service.cpp. True for a mid-stream image-write data frame (0x0071 / 0x0081)
// whose per-frame RX logging should be suppressed.
bool imageWriteLogQuietFrame(const uint8_t* data, uint16_t len);

extern "C" void od_rxq_app_report(od_rxq_event_t ev, const uint8_t *frame, uint16_t len,
                                  uint8_t depth)
{
    switch (ev) {
    case OD_RXQ_DROP_EMPTY:
        od_log_warn("WARNING: Empty BLE frame received, dropping");
        return;
    case OD_RXQ_DROP_TOO_LARGE:
        od_log_warn("WARNING: Command too large for queue (%u > %u), dropping",
                    (unsigned)len, (unsigned)OD_RXQ_FRAME_MAX);
        return;
    case OD_RXQ_DROP_FULL:
        od_log_error("ERROR: Command queue full, dropping command (%u slots)",
                     (unsigned)OD_RXQ_SLOTS);
        return;
    case OD_RXQ_ARRIVED:
        break;
    }

    if (frame == nullptr || imageWriteLogQuietFrame(frame, len)) {
        return;
    }
    {
        const uint16_t cmd = (len >= 2) ? (uint16_t)((frame[0] << 8) | frame[1]) : frame[0];
        // ERX / URX: does this frame carry the app-layer CCM envelope? Mirrors the dispatcher's
        // gate -- the two handshake opcodes are answered before it, and a frame too short to hold
        // nonce+tag cannot be wrapped. The ORIGIN_LAN_TLS term is omitted deliberately: this ring
        // is BLE only, LAN frames never reach it. Anything URX while encryption is on is rejected
        // by the dispatcher, so the token reports the frame's FORM, not its intent.
        const bool encrypted = isEncryptionEnabled() &&
                               cmd != CMD_AUTHENTICATE && cmd != CMD_FIRMWARE_VERSION &&
                               len >= BLE_CMD_HEADER_SIZE + ENCRYPTION_NONCE_SIZE + ENCRYPTION_TAG_SIZE;
        char label[48];
        char line[192];
        // Depth is the PRE-push count, matching the TX line's pre-enqueue depth, so a healthy path
        // reads [BLE][Q:0] and a rising Q means arrivals are outrunning the drain.
        snprintf(label, sizeof(label), "[BLE][Q:%u] %s 0x%04X (%u B): ",
                 (unsigned)depth, encrypted ? "ERX" : "URX", cmd, (unsigned)len);
        od_log_hex_line(line, sizeof(line), label, frame, len);
        od_log_debug("%s", line);
    }
}
