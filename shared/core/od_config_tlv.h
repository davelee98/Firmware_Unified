/* od_config_tlv.h -- walking the stored config blob, shared by every target.
 *
 * WHY THIS IS THE PART WORTH PROMOTING. The config parser is the PRE-AUTH ATTACK SURFACE: the
 * blob is parsed at boot from storage a client can write, and on a device with encryption
 * disabled the write path has no authentication gate at all. Its risk is concentrated entirely
 * in the frame walk -- the offset arithmetic and the `does this packet fit` checks -- and not
 * in the memcpy that follows. The ESP32 spelled those checks out fifteen times, once per
 * packet type, each an independent chance to write `<=` where `<` was meant.
 *
 * This module decides where a packet starts, how long it is, and whether it fits; the callback
 * receives a body whose bounds are already guaranteed. od_config.c owns the shared aggregate,
 * instance caps and outcome logging above this structural walk.
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

#include "od_span.h"
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

/* Receives ONE packet as a span of exactly its body: body.n is the declared size for packet_id
 * and every byte of it lies inside the blob. The callback cannot be handed a length that does
 * not belong to the pointer, because it is not handed a length at all -- that pairing is what
 * od_span_t exists to remove (CLAUDE.md decision 1).
 *
 * Return false to abort the walk (OD_CFG_TLV_REJECTED). Returning true for a packet the target
 * does not implement is correct and expected -- capability differences are not parse errors.
 */
typedef bool (*od_config_tlv_packet_fn)(void *ctx, uint8_t packet_id, od_span_t body);

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
 * end. The walk deliberately does no logging of its own: od_config_parse() owns the complete
 * outcome and can report the unknown id alongside its other accounting.
 */
enum od_config_tlv_result od_config_tlv_walk(od_span_t blob,
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
uint16_t od_config_tlv_crc16(od_span_t data);

#ifdef __cplusplus
}
#endif

#endif /* OD_CONFIG_TLV_H */
