/* od_config.h -- the parsed config as firmware holds it, filled once.
 *
 * WHAT THIS IS. The layer between od_config_tlv (which decides where a packet starts, how long
 * it is, and whether it fits) and the target (which acts on the result). It owns the in-memory
 * aggregate, the per-type instance caps, and the two normalisations that must be identical on
 * every chip. 2163 lines of per-target parser existed to do this: 935 in the Firmware repo's
 * targets/esp32-idf/src/config_parser.cpp, 699 in targets/nordic-zephyr, 529 in
 * targets/efr32bg22-slc, each with its own copy of the bounds checks od_config_tlv already
 * replaced and its own spelling of the storage rules below.
 *
 * FIRMWARE IS THE AUTHORITY (CLAUDE.md, migration constraints). Where the repos disagreed this
 * module takes the Firmware/esp32-idf behaviour:
 *
 *   1. THE ZERO-KEY RULE, and it is a security divergence rather than a cosmetic one. Firmware
 *      clears encryption_enabled when all 16 key bytes are zero, so such a config leaves the
 *      device honestly unencrypted. Nordic and Silabs never normalised it: their gate is a bare
 *      `sec->encryption_enabled != 0` (targets/nordic-zephyr/src/opendisplay_ble.c:246,
 *      targets/efr32bg22-slc/opendisplay_pipe.c:100), so the same config leaves those devices
 *      demanding authentication against a key any client can guess -- the worst of both, since
 *      it reads as protected. Promoted as Firmware has it.
 *
 *   2. INSTANCE CAPS SKIP, they do not overwrite. A packet arriving past the cap is dropped and
 *      counted; the earlier instances stand. All three agreed here.
 *
 * WHAT IS DELIBERATELY NOT PROMOTED. Storage (this module never reads or writes NVS; it is
 * handed a blob), all logging, and every target side effect -- the ESP32's LED re-detect and
 * its WifiConfig.server_host numeric-IP coercion, which is LAN-transport behaviour on the one
 * target that has a LAN transport. Targets keep those by acting on the per-packet result.
 *
 * Nordic's rescan_security_packet() is also NOT promoted. It scans the raw blob for an 0x27 it
 * missed, and it exists because an unknown id ends the walk: a blob carrying 0x2C on a build
 * that did not know 0x2C dropped the security packet behind it. That is a symptom of an
 * incomplete size table, and the fix belongs in the table. Porting the scan would carry a
 * workaround into shared/ and leave the next missing id silently survivable.
 *
 * PARSE AND STORE EVERY CANONICAL PACKET, whether or not this target can act on it. Settled
 * 2026-08-13 over packet 0x2A (nfc_config), which the walk's size table had omitted: using a
 * subsystem is a hardware capability, but understanding the config that describes it is a
 * protocol obligation, and a target that stops reading at a packet it cannot use makes the REST
 * of the config depend on hardware the host cannot see. So the OD_CONFIG_WITH_* gates below
 * exist to save RAM on a part that has none to spare -- not to opt a target out of a packet
 * because its hardware is absent. The ESP32 has no NFC and stores nfc_configs anyway.
 *
 * NO HAL, no allocation, no logging: the PURE tier. Reports what happened and lets the caller
 * say it.
 */
#ifndef OD_CONFIG_H
#define OD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config_tlv.h"
#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ instance caps --- */
/* Not target-tunable. All three repos independently chose these numbers and a host that writes
 * a fifth display must get the same answer from every chip; a per-target cap would be a wire
 * divergence a host cannot discover, which is the reasoning behind MAX_CONFIG_SIZE in
 * CLAUDE.md decision 12. */
#define OD_CONFIG_MAX_DISPLAYS       4u
#define OD_CONFIG_MAX_LEDS           4u
#define OD_CONFIG_MAX_SENSORS        4u
#define OD_CONFIG_MAX_DATA_BUSES     4u
#define OD_CONFIG_MAX_BINARY_INPUTS  4u
#define OD_CONFIG_MAX_TOUCH          4u
#define OD_CONFIG_MAX_BUZZERS        4u
#define OD_CONFIG_MAX_NFC            2u
#define OD_CONFIG_MAX_FLASH          2u

/* ------------------------------------------------------------------- optional subsystems --- */
/* CLAUDE.md decision 9: a target must not pay for a feature it lacks. These gate RAM, not wire
 * behaviour -- a packet for a subsystem this build lacks is still walked and still counted, it
 * is simply not stored, and the caller learns that from OD_CONFIG_APPLY_NOT_BUILT rather than
 * from silence.
 *
 * Default ON: the aggregate is the Firmware feature set, and a target that wants a smaller one
 * says so explicitly. The alternative -- default off -- makes a forgotten define look like a
 * config the host never sent, which is the harder failure to see. EFR32BG22 is the target that
 * will set these: 32 KB of RAM total, and DataExtended alone is nine 32-byte strings. */
#ifndef OD_CONFIG_WITH_TOUCH
#define OD_CONFIG_WITH_TOUCH 1
#endif
#ifndef OD_CONFIG_WITH_BUZZER
#define OD_CONFIG_WITH_BUZZER 1
#endif
#ifndef OD_CONFIG_WITH_NFC
#define OD_CONFIG_WITH_NFC 1
#endif
#ifndef OD_CONFIG_WITH_WIFI
#define OD_CONFIG_WITH_WIFI 1
#endif
#ifndef OD_CONFIG_WITH_DATA_EXTENDED
#define OD_CONFIG_WITH_DATA_EXTENDED 1
#endif

/* --------------------------------------------------------------------------- the aggregate --- */
/* NOT a wire struct: unpacked, our field order, and the counts and `loaded` flags have no
 * on-wire representation. The wire form is the TLV blob od_config_tlv walks. */
struct od_config {
    struct SystemConfig      system_config;
    struct ManufacturerData  manufacturer_data;
    struct PowerOption       power_option;

    struct DisplayConfig     displays[OD_CONFIG_MAX_DISPLAYS];
    uint8_t                  display_count;
    struct LedConfig         leds[OD_CONFIG_MAX_LEDS];
    uint8_t                  led_count;
    struct SensorData        sensors[OD_CONFIG_MAX_SENSORS];
    uint8_t                  sensor_count;
    struct DataBus           data_buses[OD_CONFIG_MAX_DATA_BUSES];
    uint8_t                  data_bus_count;
    struct BinaryInputs      binary_inputs[OD_CONFIG_MAX_BINARY_INPUTS];
    uint8_t                  binary_input_count;
    struct FlashConfig       flash_configs[OD_CONFIG_MAX_FLASH];
    uint8_t                  flash_config_count;

#if OD_CONFIG_WITH_TOUCH
    struct TouchController   touch_controllers[OD_CONFIG_MAX_TOUCH];
    uint8_t                  touch_controller_count;
#endif
#if OD_CONFIG_WITH_BUZZER
    /* Canonical spelling is BuzzerConfig; the member keeps the passive_buzzers name every
     * existing parser and caller already uses. */
    struct BuzzerConfig      passive_buzzers[OD_CONFIG_MAX_BUZZERS];
    uint8_t                  passive_buzzer_count;
#endif
#if OD_CONFIG_WITH_NFC
    struct NfcConfig         nfc_configs[OD_CONFIG_MAX_NFC];
    uint8_t                  nfc_config_count;
#endif
#if OD_CONFIG_WITH_DATA_EXTENDED
    struct DataExtended      data_extended;
    bool                     data_extended_loaded;
#endif
#if OD_CONFIG_WITH_WIFI
    /* Stored verbatim. The ESP32's server_host coercion and its credential copies stay in that
     * target -- they are LAN-transport behaviour, not config parsing. */
    struct WifiConfig        wifi_config;
    bool                     wifi_config_loaded;
#endif

    /* Held here rather than in a target global, which is where all three keep it today, so the
     * zero-key normalisation cannot be separated from the parse that must apply it. */
    struct SecurityConfig    security;
    bool                     security_loaded;

    uint8_t                  version;         /* outer version byte */
    uint8_t                  minor_version;   /* always 0: not carried by the current format */
    bool                     loaded;
};

/* --------------------------------------------------------------------------------- results --- */
enum od_config_apply {
    OD_CONFIG_APPLY_STORED = 0,
    OD_CONFIG_APPLY_FULL,        /* instance cap reached; this packet dropped, earlier kept */
    OD_CONFIG_APPLY_NOT_BUILT,   /* a real wire packet this build has no storage for */
    OD_CONFIG_APPLY_UNKNOWN_ID,  /* not a packet id this build knows at all */
    OD_CONFIG_APPLY_SHORT_BODY   /* body smaller than the struct -- never from the walk */
};

/* Everything the target used to log from inside its own parser. Filled by od_config_parse();
 * shared/ has no log seam and a kernel-free target may have nowhere to send one. */
struct od_config_report {
    uint16_t stored;             /* packets stored */
    uint16_t dropped_full;       /* packets dropped at an instance cap */
    uint16_t dropped_not_built;  /* packets for subsystems this build lacks */
    uint8_t  unknown_id;         /* id that ended the walk, or 0 if it ran to the end */

    /* ADVISORY, exactly as the fleet has it: computed and reported, never enforced. Making the
     * CRC authoritative would reject configs every shipped device accepts today -- a decision
     * to take deliberately, not as a side effect of this promotion. */
    bool     crc_checked;
    uint16_t crc_stored;
    uint16_t crc_computed;
};

/* ------------------------------------------------------------------------------ interface --- */

/* Zero every field. Equivalent to the memset each target opens its load with. */
void od_config_reset(struct od_config *cfg);

/* True when any of the 16 key bytes is non-zero. Exposed because targets ask it outside the
 * parse (Nordic's od_security_key_set), and because it is half of the zero-key rule. */
bool od_config_security_key_set(const struct SecurityConfig *sec);

/* Store one packet whose bounds the walk has already guaranteed. Safe to call directly for a
 * target that keeps its own walk during migration. */
enum od_config_apply od_config_apply_packet(struct od_config *cfg, uint8_t packet_id,
                                            const uint8_t *body, uint16_t body_len);

/* Reset, walk, store, then compute the advisory CRC.
 *
 * cfg->loaded is set only on OD_CFG_TLV_OK, matching Firmware's loadGlobalConfig(): a blob that
 * is too short or truncated leaves a zeroed config marked not-loaded, so a caller that ignores
 * the result still cannot read a half-parsed config as a real one. report may be NULL.
 */
enum od_config_tlv_result od_config_parse(struct od_config *cfg, const uint8_t *blob,
                                          uint32_t len, struct od_config_report *report);

#ifdef __cplusplus
}
#endif

#endif /* OD_CONFIG_H */
