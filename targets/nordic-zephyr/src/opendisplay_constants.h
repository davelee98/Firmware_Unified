#ifndef OPENDISPLAY_CONSTANTS_H
#define OPENDISPLAY_CONSTANTS_H

/* Target-local constants ONLY.
 *
 * Eleven definitions left this file when the target adopted the canonical wire contract
 * (shared/protocol/): CONFIG_CHUNK_SIZE, CONFIG_CHUNK_SIZE_WITH_PREFIX, MAX_CONFIG_CHUNKS,
 * MAX_RESPONSE_DATA_SIZE, OD_BUS_TYPE_I2C, OD_NFC_IC_AUTO, OD_NFC_IC_TNB132M and the four
 * OD_NFC_REC_*. Every one had the same value canonical carries, so nothing changed but the
 * source of truth.
 *
 * OD_BUS_TYPE_I2C is why this is a rule and not a tidy-up. Canonical declares it as an
 * ENUMERATOR; a macro of the same spelling textually rewrites the enumerator and the header
 * fails to compile -- "expected identifier before numeric constant" pointing at this file, not
 * at the enum. The canonical header warns about exactly this for OD_NFC_IC_*. So: do not add a
 * definition here for anything shared/protocol/ names, even with a matching value.
 */

#define CONFIG_PKT_SYSTEM         0x01
#define CONFIG_PKT_MANUFACTURER   0x02
#define CONFIG_PKT_POWER          0x04
#define CONFIG_PKT_DISPLAY        0x20
#define CONFIG_PKT_LED            0x21
#define CONFIG_PKT_SENSOR         0x23
#define CONFIG_PKT_DATA_BUS       0x24
#define CONFIG_PKT_BINARY_INPUT   0x25
#define CONFIG_PKT_WIFI           0x26
#define CONFIG_PKT_SECURITY       0x27
#define CONFIG_PKT_TOUCH          0x28
#define CONFIG_PKT_PASSIVE_BUZZER 0x29
#define CONFIG_PKT_NFC            0x2A
#define CONFIG_PKT_FLASH          0x2B
#define CONFIG_PKT_DATA_EXTENDED  0x2C

/* CONFIG_CHUNK_SIZE, CONFIG_CHUNK_SIZE_WITH_PREFIX, MAX_CONFIG_CHUNKS and
 * MAX_RESPONSE_DATA_SIZE now come from shared/protocol/opendisplay_protocol.h. */

#define GPIO_PIN_UNUSED 0xFF

/* OD_BUS_TYPE_I2C (enum BusType), OD_NFC_IC_AUTO / OD_NFC_IC_TNB132M and the OD_NFC_REC_*
 * record types now come from shared/protocol/. OD_NFC_IC_SOC_NFCT does not exist there and
 * lives in protocol_pending.h with the sequence for landing it upstream. */

/* transmission_modes bitfield, per toolbox config.yaml:
 *   bit0 streaming_decompression - streaming zlib inflate, 512-byte DEFLATE window
 *   bit1 zip                     - zip compressed transfer (full window)
 *   bit2 g5                      - group 5 compression (not implemented)
 *   bit3 direct_write            - bufferless direct write
 *   bit4 pipe_write              - sliding-window PIPE_WRITE (0x80-0x82)
 * This firmware implements the 512-byte streaming inflater (od_zlib_stream), so
 * compressed direct/pipe writes are gated on the streaming_decompression bit. */
#define TRANSMISSION_MODE_STREAMING_DECOMPRESSION (1u << 0)
#define TRANSMISSION_MODE_ZIP                     (1u << 1)
#define TRANSMISSION_MODE_G5                      (1u << 2)
#define TRANSMISSION_MODE_DIRECT_WRITE            (1u << 3)
#define TRANSMISSION_MODE_PIPE_WRITE              (1u << 4)
#define TRANSMISSION_MODE_CLEAR_ON_BOOT           (1u << 7)

#endif
