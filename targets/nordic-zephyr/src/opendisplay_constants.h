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


/* CONFIG_CHUNK_SIZE, CONFIG_CHUNK_SIZE_WITH_PREFIX, MAX_CONFIG_CHUNKS and
 * MAX_RESPONSE_DATA_SIZE now come from shared/protocol/opendisplay_protocol.h. */

/* OD_BUS_TYPE_I2C (enum BusType), OD_NFC_IC_AUTO / OD_NFC_IC_TNB132M and the OD_NFC_REC_*
 * record types now come from shared/protocol/. OD_NFC_IC_SOC_NFCT does not exist there and
 * lives in protocol_pending.h with the sequence for landing it upstream. */

/* The GPIO_PIN_UNUSED and TRANSMISSION_MODE_* spellings are gone: shared/protocol/ names both
 * sets canonically as OD_PIN_UNUSED and OD_TRANSMISSION_MODE_*, and a second definition of a
 * wire constant is what the note at the top of this file forbids. */

#endif
