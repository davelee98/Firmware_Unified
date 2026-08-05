/* od_config_tlv.h -- walking the stored config blob, shared by every target.
 *
 * WHY THIS IS THE PART WORTH PROMOTING. The config parser is the PRE-AUTH ATTACK SURFACE: the
 * blob is parsed at boot from storage a client can write, and on a device with encryption
 * disabled the write path has no authentication gate at all. Its risk is concentrated entirely
 * in the frame walk -- the offset arithmetic and the `does this packet fit` checks -- and not
 * in the memcpy that follows. The ESP32 spelled those checks out fifteen times, once per
 * packet type, each an independent chance to write `<=` where `<` was meant.
 *
 * So the WALK is shared and the COPY is not. This module decides where a packet starts, how
 * long it is, and whether it fits; the target's callback receives a body whose bounds are
 * already guaranteed and puts it wherever that target keeps it. Per-target aggregation
 * structures, instance-count caps and logging stay where they belong.
 *
 * WHAT THIS DELIBERATELY DOES NOT CHANGE. Unknown packet IDs still abandon the rest of the
 * blob (skip-to-CRC), exactly as every shipped target does today. The size-table skip model
 * that would let an old device step over a NEW packet type and keep reading is an INCOMPATIBLE
 * WIRE CHANGE and is deferred (NEXT_STEPS_2026-08-05.md D4). The table below is arranged so
 * that model can be switched on later without re-cutting the walker -- but it is not switched
 * on, and a partial version must not arrive as a side effect of this promotion.
 *
 * Plain C99. No allocation, no storage access, no logging: this module reads a buffer and
 * reports. It cannot alter what it parses.
 */
#ifndef OD_CONFIG_TLV_H
#define OD_CONFIG_TLV_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The outer blob is [reserved:2][version:1] ... [crc:2 LE]. Both ends are fixed overhead the
 * walk must not mistake for packet data -- getting the trailing two wrong is how a CRC gets
 * parsed as a packet header. */
#define OD_CFG_TLV_HEADER_LEN 3u
#define OD_CFG_TLV_CRC_LEN    2u

enum od_config_tlv_result {
    OD_CFG_TLV_OK = 0,
    OD_CFG_TLV_TOO_SHORT,    /* smaller than header + CRC; nothing to parse */
    OD_CFG_TLV_TRUNCATED,    /* a packet claims more bytes than the blob holds */
    OD_CFG_TLV_REJECTED      /* the callback refused a packet */
};

/* Receives ONE packet with its bounds already guaranteed: body_len is the declared size for
 * packet_id and body..body+body_len-1 lies inside the blob.
 *
 * Return false to abort the walk (OD_CFG_TLV_REJECTED). Returning true for a packet the target
 * does not implement is correct and expected -- capability differences are not parse errors.
 */
typedef bool (*od_config_tlv_packet_fn)(void *ctx, uint8_t packet_id,
                                        const uint8_t *body, uint16_t body_len);

/* Declared body size for a packet id, or 0 if this build does not know the id.
 *
 * Exposed because the target's unknown-packet policy needs it and because it is the table a
 * future size-table skip model turns on. Sizes come from the canonical structs, never from a
 * hand-written number.
 */
uint16_t od_config_tlv_body_size(uint8_t packet_id);

/* Walk the blob, calling fn once per recognised packet.
 *
 * version_out receives the outer version byte when the blob is long enough to have one.
 *
 * UNKNOWN IDS END THE WALK, and that is current fleet behaviour, not an oversight -- see the
 * header comment. The walk stops cleanly and returns OD_CFG_TLV_OK: a config carrying a packet
 * this build does not know is not corrupt, it is newer.
 *
 * unknown_id_out, when supplied, receives the id that ended the walk, or 0 if it ran to the
 * end. It exists so the caller can keep logging WHICH id stopped it: the per-target parser used
 * to warn "Unknown packet ID 0x%02X", and losing that in the promotion would trade a diagnostic
 * for nothing. The walk deliberately does no logging of its own -- shared/ has no log seam and
 * a kernel-free target may have nowhere to send it.
 */
enum od_config_tlv_result od_config_tlv_walk(const uint8_t *blob, uint32_t len,
                                             od_config_tlv_packet_fn fn, void *ctx,
                                             uint8_t *version_out,
                                             uint8_t *unknown_id_out);

/* CRC-16/CCITT over the body, matching the toolbox, nRF and Silabs firmware.
 *
 * ADVISORY ONLY at every call site today: a mismatch is warned about, never enforced. Promoted
 * as-is rather than tightened, because making it authoritative is a behaviour change that would
 * reject configs currently accepted across the whole fleet -- a deliberate decision to take
 * separately, not a side effect of moving the function.
 */
uint16_t od_config_tlv_crc16(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* OD_CONFIG_TLV_H */
