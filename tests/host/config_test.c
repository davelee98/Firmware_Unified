/* config_test.c -- host tests for the shared config aggregate.
 *
 * This module is downstream of the pre-auth attack surface: od_config_tlv guarantees the bounds,
 * and everything here is what happens to a body once those bounds hold. So the cases that matter
 * are the ones where a hostile or merely unusual blob still reaches storage -- an instance array
 * driven past its cap, a 32-byte string field with no terminator, a security packet whose key is
 * all zeros, and a truncated blob that must leave a ZEROED config rather than a half-filled one.
 *
 * The Firmware repo is the authority (CLAUDE.md), so where the source parsers disagreed the
 * expectations below are Firmware's: the zero-key rule and skip-not-overwrite at the caps.
 */
#include "od_config.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);  \
        }                                                                       \
    } while (0)

#define CASE(name) (g_case = (name))

/* ---------------------------------------------------------------------------- blob builder --- */

/* [reserved:2][version:1] packets... [crc:2 LE], each packet [reserved:1][id:1][body]. */
struct blob {
    uint8_t  bytes[2048];
    uint32_t len;
};

static void blob_start(struct blob *b, uint8_t version)
{
    memset(b, 0, sizeof(*b));
    b->bytes[0] = 0x00;
    b->bytes[1] = 0x00;
    b->bytes[2] = version;
    b->len = 3u;
}

static void blob_add(struct blob *b, uint8_t id, const uint8_t *body, uint16_t body_len)
{
    b->bytes[b->len++] = 0x00;      /* reserved */
    b->bytes[b->len++] = id;
    if (body != NULL) {
        memcpy(&b->bytes[b->len], body, body_len);
    }
    b->len += body_len;
}

/* Real CRC by default: the parse must not depend on it, but a test blob that carries a wrong one
 * everywhere would hide a report field that is meant to be usable. */
static void blob_finish(struct blob *b, bool valid_crc)
{
    uint16_t crc = od_config_tlv_crc16(b->bytes, b->len);

    if (!valid_crc) {
        crc = (uint16_t)(crc ^ 0xFFFFu);
    }
    b->bytes[b->len++] = (uint8_t)(crc & 0xFFu);
    b->bytes[b->len++] = (uint8_t)(crc >> 8);
}

/* A body of `size` bytes filled with a recognisable pattern. */
static void fill_body(uint8_t *body, uint16_t size, uint8_t seed)
{
    uint16_t i;

    for (i = 0; i < size; ++i) {
        body[i] = (uint8_t)(seed + i);
    }
}

/* -------------------------------------------------------------------------------- the tests --- */

static void test_single_instance_packets(void)
{
    struct od_config cfg;
    struct od_config_report rep;
    struct blob b;
    uint8_t body[512];
    enum od_config_tlv_result r;

    CASE("single-instance packets land in their slots");
    blob_start(&b, 2u);
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10);
    blob_add(&b, 0x01, body, (uint16_t)sizeof(struct SystemConfig));
    fill_body(body, (uint16_t)sizeof(struct ManufacturerData), 0x20);
    blob_add(&b, 0x02, body, (uint16_t)sizeof(struct ManufacturerData));
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x30);
    blob_add(&b, 0x04, body, (uint16_t)sizeof(struct PowerOption));
    blob_finish(&b, true);

    r = od_config_parse(&cfg, b.bytes, b.len, &rep);
    CHECK(r == OD_CFG_TLV_OK);
    CHECK(cfg.loaded);
    CHECK(cfg.version == 2u);
    CHECK(cfg.minor_version == 0u);
    CHECK(rep.stored == 3u);
    CHECK(rep.unknown_id == 0u);

    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10);
    CHECK(memcmp(&cfg.system_config, body, sizeof(struct SystemConfig)) == 0);
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x30);
    CHECK(memcmp(&cfg.power_option, body, sizeof(struct PowerOption)) == 0);

    CASE("the advisory CRC is reported, not enforced");
    CHECK(rep.crc_checked);
    CHECK(rep.crc_stored == rep.crc_computed);

    blob_start(&b, 1u);
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10);
    blob_add(&b, 0x01, body, (uint16_t)sizeof(struct SystemConfig));
    blob_finish(&b, false);
    r = od_config_parse(&cfg, b.bytes, b.len, &rep);
    CHECK(r == OD_CFG_TLV_OK);      /* a bad CRC must still parse */
    CHECK(cfg.loaded);
    CHECK(rep.crc_stored != rep.crc_computed);
}

static void test_instance_caps(void)
{
    struct od_config cfg;
    struct od_config_report rep;
    struct blob b;
    uint8_t body[512];
    unsigned i;

    /* Six displays into an array of four. Firmware keeps the first four and drops the rest; the
     * alternative -- overwriting the last slot -- would let a host silently replace a display it
     * never addressed. */
    CASE("an instance array saturates by skipping, not overwriting");
    blob_start(&b, 1u);
    for (i = 0; i < 6u; ++i) {
        fill_body(body, (uint16_t)sizeof(struct DisplayConfig), (uint8_t)(0x40u + i));
        blob_add(&b, 0x20, body, (uint16_t)sizeof(struct DisplayConfig));
    }
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, &rep) == OD_CFG_TLV_OK);
    CHECK(cfg.display_count == OD_CONFIG_MAX_DISPLAYS);
    CHECK(rep.stored == OD_CONFIG_MAX_DISPLAYS);
    CHECK(rep.dropped_full == 2u);

    /* The kept four are the FIRST four, in order. */
    for (i = 0; i < OD_CONFIG_MAX_DISPLAYS; ++i) {
        fill_body(body, (uint16_t)sizeof(struct DisplayConfig), (uint8_t)(0x40u + i));
        CHECK(memcmp(&cfg.displays[i], body, sizeof(struct DisplayConfig)) == 0);
    }

    CASE("a saturated array does not stop the packets behind it");
    blob_start(&b, 1u);
    for (i = 0; i < 6u; ++i) {
        fill_body(body, (uint16_t)sizeof(struct DisplayConfig), (uint8_t)(0x40u + i));
        blob_add(&b, 0x20, body, (uint16_t)sizeof(struct DisplayConfig));
    }
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x77);
    blob_add(&b, 0x04, body, (uint16_t)sizeof(struct PowerOption));
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, &rep) == OD_CFG_TLV_OK);
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x77);
    CHECK(memcmp(&cfg.power_option, body, sizeof(struct PowerOption)) == 0);
}

static void test_security_zero_key(void)
{
    struct od_config cfg;
    struct blob b;
    struct SecurityConfig sec;

    /* THE FIRMWARE RULE. encryption_enabled is set, the key is all zeros: the device must end up
     * unencrypted rather than authenticating against a key any client can guess. Nordic and
     * Silabs skip this normalisation today, which is the divergence this promotion settles. */
    CASE("an all-zero key disables encryption however the flag is set");
    memset(&sec, 0, sizeof sec);
    sec.encryption_enabled = 1u;
    sec.session_timeout_seconds = 300u;
    blob_start(&b, 1u);
    blob_add(&b, 0x27, (const uint8_t *)&sec, (uint16_t)sizeof sec);
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, NULL) == OD_CFG_TLV_OK);
    CHECK(cfg.security_loaded);
    CHECK(cfg.security.encryption_enabled == 0u);
    CHECK(!od_config_security_key_set(&cfg.security));
    /* Everything else in the packet survives -- only the flag is normalised. */
    CHECK(cfg.security.session_timeout_seconds == 300u);

    CASE("a real key leaves the flag alone");
    memset(&sec, 0, sizeof sec);
    sec.encryption_enabled = 1u;
    sec.encryption_key[15] = 0x01u;    /* the last byte only: a memcmp-based check must catch it */
    blob_start(&b, 1u);
    blob_add(&b, 0x27, (const uint8_t *)&sec, (uint16_t)sizeof sec);
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, NULL) == OD_CFG_TLV_OK);
    CHECK(cfg.security.encryption_enabled == 1u);
    CHECK(od_config_security_key_set(&cfg.security));

    CASE("a key set with the flag clear stays disabled");
    memset(&sec, 0, sizeof sec);
    sec.encryption_enabled = 0u;
    sec.encryption_key[0] = 0xA5u;
    blob_start(&b, 1u);
    blob_add(&b, 0x27, (const uint8_t *)&sec, (uint16_t)sizeof sec);
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, NULL) == OD_CFG_TLV_OK);
    CHECK(cfg.security.encryption_enabled == 0u);
    CHECK(od_config_security_key_set(&cfg.security));
}

#if OD_CONFIG_WITH_DATA_EXTENDED
static void test_data_extended_termination(void)
{
    struct od_config cfg;
    struct blob b;
    struct DataExtended de;

    /* A host may fill all 32 bytes of a name field. Without the terminator every consumer that
     * prints one runs into the next field; Firmware terminates all nine at parse time. */
    CASE("every string field is terminated even when the host fills it");
    memset(&de, 'A', sizeof de);
    blob_start(&b, 1u);
    blob_add(&b, 0x2C, (const uint8_t *)&de, (uint16_t)sizeof de);
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, NULL) == OD_CFG_TLV_OK);
    CHECK(cfg.data_extended_loaded);
    CHECK(cfg.data_extended.manufacturer_name[31] == '\0');
    CHECK(cfg.data_extended.model_name[31] == '\0');
    CHECK(cfg.data_extended.serial_number[31] == '\0');
    CHECK(cfg.data_extended.friendly_name[31] == '\0');
    CHECK(cfg.data_extended.device_location[31] == '\0');
    CHECK(cfg.data_extended.device_id[31] == '\0');
    CHECK(cfg.data_extended.custom_string_1[31] == '\0');
    CHECK(cfg.data_extended.custom_string_2[31] == '\0');
    CHECK(cfg.data_extended.custom_string_3[31] == '\0');
    /* The canonical fields are uint8_t[32], not char[32], so measure the run rather than
     * reaching for strlen -- the cast that would silence the compiler here is the same cast a
     * consumer writes before printing, and it is the terminator that makes it safe. */
    CHECK(strlen((const char *)cfg.data_extended.manufacturer_name) == 31u);
}
#endif

static void test_hostile_blobs(void)
{
    struct od_config cfg;
    struct od_config_report rep;
    struct blob b;
    uint8_t body[512];

    CASE("a truncated packet leaves a zeroed config, not a half-filled one");
    blob_start(&b, 1u);
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10);
    blob_add(&b, 0x01, body, (uint16_t)sizeof(struct SystemConfig));
    /* A display header whose body runs off the end of the blob. */
    blob_add(&b, 0x20, NULL, 4u);
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, &rep) == OD_CFG_TLV_TRUNCATED);
    CHECK(!cfg.loaded);
    CHECK(cfg.display_count == 0u);
    /* STATED PLAINLY because it is the contract, not an accident: packets that parsed BEFORE the
     * truncation ARE still in the struct -- the walk applies as it goes and cannot unwind. What
     * the reset guarantees is that nothing from a PREVIOUS parse survives; what marks this one
     * unusable is loaded == false. A caller that reads fields without checking loaded gets a
     * partial config, which is why every target's load path must gate on it. */
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10);
    CHECK(memcmp(&cfg.system_config, body, sizeof(struct SystemConfig)) == 0);

    CASE("a blob too short to hold a version and a CRC is refused");
    memset(&b, 0, sizeof b);
    b.len = 4u;
    CHECK(od_config_parse(&cfg, b.bytes, b.len, &rep) == OD_CFG_TLV_TOO_SHORT);
    CHECK(!cfg.loaded);

    CASE("an unknown id ends the walk and is reported, without failing the parse");
    blob_start(&b, 1u);
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x30);
    blob_add(&b, 0x04, body, (uint16_t)sizeof(struct PowerOption));
    blob_add(&b, 0x7E, body, 4u);     /* not a canonical id */
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10);
    blob_add(&b, 0x01, body, (uint16_t)sizeof(struct SystemConfig));
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, b.bytes, b.len, &rep) == OD_CFG_TLV_OK);
    CHECK(cfg.loaded);
    CHECK(rep.unknown_id == 0x7Eu);
    CHECK(rep.stored == 1u);
    /* The packet BEHIND the unknown id was abandoned -- current fleet behaviour, preserved. */
    CHECK(cfg.system_config.device_flags == 0u);

    CASE("a NULL blob and a NULL config are refused, not dereferenced");
    CHECK(od_config_parse(&cfg, NULL, 100u, &rep) == OD_CFG_TLV_TOO_SHORT);
    CHECK(od_config_parse(NULL, b.bytes, b.len, &rep) == OD_CFG_TLV_TOO_SHORT);
    CHECK(od_config_parse(&cfg, b.bytes, b.len, NULL) == OD_CFG_TLV_OK);   /* NULL report is fine */
}

static void test_apply_directly(void)
{
    struct od_config cfg;
    uint8_t body[512];
    unsigned i;

    /* od_config_apply_packet is callable without the walk, which is how a target that still has
     * its own walk adopts this module first. */
    CASE("apply is usable without the walk");
    od_config_reset(&cfg);
    fill_body(body, (uint16_t)sizeof(struct LedConfig), 0x55);
    CHECK(od_config_apply_packet(&cfg, 0x21, body, (uint16_t)sizeof(struct LedConfig))
          == OD_CONFIG_APPLY_STORED);
    CHECK(cfg.led_count == 1u);

    CASE("a body shorter than the struct is refused rather than over-read");
    od_config_reset(&cfg);
    CHECK(od_config_apply_packet(&cfg, 0x21, body, 2u) == OD_CONFIG_APPLY_SHORT_BODY);
    CHECK(cfg.led_count == 0u);
    CHECK(od_config_apply_packet(&cfg, 0x01, body, 2u) == OD_CONFIG_APPLY_SHORT_BODY);

    CASE("an id this table does not know is reported, not stored");
    CHECK(od_config_apply_packet(&cfg, 0x7E, body, 64u) == OD_CONFIG_APPLY_UNKNOWN_ID);

    CASE("NULL arguments are refused");
    CHECK(od_config_apply_packet(NULL, 0x21, body, 64u) == OD_CONFIG_APPLY_SHORT_BODY);
    CHECK(od_config_apply_packet(&cfg, 0x21, NULL, 64u) == OD_CONFIG_APPLY_SHORT_BODY);

    CASE("reset clears every count");
    od_config_reset(&cfg);
    for (i = 0; i < 3u; ++i) {
        fill_body(body, (uint16_t)sizeof(struct SensorData), (uint8_t)i);
        (void)od_config_apply_packet(&cfg, 0x23, body, (uint16_t)sizeof(struct SensorData));
    }
    CHECK(cfg.sensor_count == 3u);
    od_config_reset(&cfg);
    CHECK(cfg.sensor_count == 0u);
    CHECK(!cfg.loaded);
    CHECK(!cfg.security_loaded);

    /* 0x2A is stored here even though od_config_tlv's table does not yet know it, so settling
     * that wire question moves one line in the table and nothing in this module. */
    CASE("nfc_config is storable ahead of the walk knowing the id");
    od_config_reset(&cfg);
    fill_body(body, (uint16_t)sizeof(struct NfcConfig), 0x60);
#if OD_CONFIG_WITH_NFC
    CHECK(od_config_apply_packet(&cfg, 0x2A, body, (uint16_t)sizeof(struct NfcConfig))
          == OD_CONFIG_APPLY_STORED);
    CHECK(cfg.nfc_config_count == 1u);
#else
    CHECK(od_config_apply_packet(&cfg, 0x2A, body, (uint16_t)sizeof(struct NfcConfig))
          == OD_CONFIG_APPLY_NOT_BUILT);
#endif
}

int main(void)
{
    test_single_instance_packets();
    test_instance_caps();
    test_security_zero_key();
#if OD_CONFIG_WITH_DATA_EXTENDED
    test_data_extended_termination();
#endif
    test_hostile_blobs();
    test_apply_directly();

    printf("config: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
