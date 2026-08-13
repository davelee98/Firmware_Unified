/* od_config_tlv.c -- see od_config_tlv.h for why the walk is shared and the copy is not. */
#include "od_config_tlv.h"

uint16_t od_config_tlv_body_size(uint8_t packet_id)
{
    /* EVERY SIZE IS A sizeof, never a literal. The canonical structs already carry
     * OD_STATIC_ASSERT wire-size checks, so a struct that changes shape fails at compile time
     * in the header rather than silently shifting every offset in this walk. */
    switch (packet_id) {
    case 0x01: return (uint16_t)sizeof(struct SystemConfig);
    case 0x02: return (uint16_t)sizeof(struct ManufacturerData);
    case 0x04: return (uint16_t)sizeof(struct PowerOption);
    case 0x20: return (uint16_t)sizeof(struct DisplayConfig);
    case 0x21: return (uint16_t)sizeof(struct LedConfig);
    case 0x23: return (uint16_t)sizeof(struct SensorData);
    case 0x24: return (uint16_t)sizeof(struct DataBus);
    case 0x25: return (uint16_t)sizeof(struct BinaryInputs);
    case 0x26: return (uint16_t)sizeof(struct WifiConfig);
    case 0x27: return (uint16_t)sizeof(struct SecurityConfig);
    case 0x28: return (uint16_t)sizeof(struct TouchController);
    case 0x29: return (uint16_t)sizeof(struct BuzzerConfig);
    /* 0x2A nfc_config. ADDED 2026-08-13 BY DECISION, not as a table fix -- it changes the wire
     * on the ESP32, which previously treated the id as unknown and abandoned the remainder of
     * the blob. The decision: a target parses and stores every canonical packet whether or not
     * it can act on it. Using a subsystem is a hardware capability; understanding the config
     * that describes it is a protocol obligation, and a target that stops reading at a packet it
     * cannot use makes the REST of the config depend on hardware the host cannot see.
     *
     * What changes on the ESP32: 0x2B and 0x2C packets that follow an nfc_config are now applied
     * instead of discarded -- i.e. flash_config and data_extended start taking effect on devices
     * whose config carries NFC. That is a fix in substance, since the host wrote those packets
     * and had no way to know they were being dropped. What changes on Nordic and Silabs: nothing
     * -- both already parse 0x2A and continue, which is why leaving it out would have regressed
     * them the moment either adopted this walk.
     *
     * This is NOT the deferred size-table skip model (D4). That is about ids unknown to the
     * CANONICAL contract; this id has always been in it. */
    case 0x2A: return (uint16_t)sizeof(struct NfcConfig);
    case 0x2B: return (uint16_t)sizeof(struct FlashConfig);
    case 0x2C: return (uint16_t)sizeof(struct DataExtended);
    default:   return 0u;
    }
}

enum od_config_tlv_result od_config_tlv_walk(const uint8_t *blob, uint32_t len,
                                             od_config_tlv_packet_fn fn, void *ctx,
                                             uint8_t *version_out, uint8_t *unknown_id_out)
{
    if (unknown_id_out) {
        *unknown_id_out = 0u;
    }
    if (!blob || !fn) {
        return OD_CFG_TLV_TOO_SHORT;
    }
    if (len < OD_CFG_TLV_HEADER_LEN + OD_CFG_TLV_CRC_LEN) {
        /* Not enough for a version byte and a CRC, so there is no blob here to disagree
         * about. The old check was `configLen < 3`, which admitted a 3- or 4-byte input whose
         * "body end" (len - 2) sat at or before the header -- the loop then simply did not run
         * and the config was reported LOADED. Refusing it is the honest answer. */
        return OD_CFG_TLV_TOO_SHORT;
    }

    if (version_out) {
        *version_out = blob[2];
    }

    /* The one bound every packet is measured against. Computed ONCE, where the old parser
     * recomputed `configLen - 2` at fifteen separate comparison sites. */
    const uint32_t body_end = len - OD_CFG_TLV_CRC_LEN;

    uint32_t offset = OD_CFG_TLV_HEADER_LEN;
    while (offset < body_end) {
        /* Each packet is [reserved:1][id:1][body...]. Both header bytes must be inside the
         * body region -- reading an id out of the trailing CRC is exactly the confusion the
         * single body_end above exists to prevent. */
        if (offset + 2u > body_end) {
            break;
        }
        offset++;                              /* reserved */
        const uint8_t packet_id = blob[offset++];

        const uint16_t body_size = od_config_tlv_body_size(packet_id);
        if (body_size == 0u) {
            /* Unknown id: abandon the remainder. Current fleet behaviour, preserved
             * deliberately -- the size-table skip that would let the walk continue is an
             * incompatible wire change and is deferred (D4). Not an error: a config carrying a
             * packet this build does not know is newer, not corrupt.
             *
             * HISTORY, kept because it is the one case that showed what this branch costs. This
             * comment used to say 0x2A (nfc_config) was deliberately absent from the table as "a
             * known packet no target here implements". That was wrong on its facts -- Nordic and
             * Silabs both parsed it and continued -- so the table encoded the ESP32's behaviour
             * as if it were the fleet's, and this branch silently made the rest of a config
             * depend on whether the device had NFC hardware. Settled 2026-08-13: every canonical
             * packet is parsed and stored whether or not the target can act on it, so 0x2A is in
             * the table above and only genuinely-unknown ids reach here. */
            if (unknown_id_out) {
                *unknown_id_out = packet_id;
            }
            return OD_CFG_TLV_OK;
        }

        /* THE CHECK THIS MODULE EXISTS FOR, written once. Subtraction rather than
         * `offset + body_size <= body_end` so a large declared size cannot overflow the sum
         * and turn a truncated blob into an in-bounds read. */
        if ((uint32_t)body_size > body_end - offset) {
            return OD_CFG_TLV_TRUNCATED;
        }

        if (!fn(ctx, packet_id, &blob[offset], body_size)) {
            return OD_CFG_TLV_REJECTED;
        }
        offset += body_size;
    }

    return OD_CFG_TLV_OK;
}

static uint16_t od_crc16_feed(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)((uint16_t)b << 8);
    for (unsigned bit = 0; bit < 8u; ++bit) {
        if ((crc & 0x8000u) != 0u) {
            crc = (uint16_t)(((uint32_t)crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)((uint32_t)crc << 1);
        }
    }
    return crc;
}

uint16_t od_config_tlv_crc16(const uint8_t *data, uint32_t len)
{
    /* CRC-16/CCITT-FALSE: init 0xFFFF, poly 0x1021, no reflection, no final xor.
     *
     * THE FIRST TWO BYTES ARE FED AS ZERO, and that is not an accident to be tidied away. Those
     * are the container's length field, and zeroing them makes the checksum independent of the
     * declared length -- which is what the toolbox, the nRF firmware and the Silabs firmware
     * all do. Computing a plain CRC over the whole body here would be a subtly different
     * function that disagreed with every shipped device about which configs are intact, and it
     * would present as a warning that appears from nowhere rather than as an obvious break.
     *
     * Byte-at-a-time on purpose: the targets that run this have no table to spare. */
    uint16_t crc = 0xFFFFu;
    if (!data) {
        return crc;
    }
    if (len < 2u) {
        for (uint32_t i = 0; i < len; ++i) {
            crc = od_crc16_feed(crc, data[i]);
        }
        return crc;
    }
    crc = od_crc16_feed(crc, 0u);
    crc = od_crc16_feed(crc, 0u);
    for (uint32_t i = 2u; i < len; ++i) {
        crc = od_crc16_feed(crc, data[i]);
    }
    return crc;
}
