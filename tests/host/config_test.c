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
#include "od_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define LOG_CAP 256u
#define LOG_TEXT_CAP 256u

struct captured_log {
    int level;
    char text[LOG_TEXT_CAP];
};

static struct captured_log g_logs[LOG_CAP];
static unsigned g_log_count;

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    if (g_log_count >= LOG_CAP) {
        return;
    }
    g_logs[g_log_count].level = level;
    va_start(ap, fmt);
    (void)vsnprintf(g_logs[g_log_count].text, sizeof(g_logs[g_log_count].text), fmt, ap);
    va_end(ap);
    ++g_log_count;
}

static void logs_reset(void)
{
    memset(g_logs, 0, sizeof(g_logs));
    g_log_count = 0u;
}

static bool logged_exact(int level, const char *text)
{
    unsigned i;

    for (i = 0u; i < g_log_count; ++i) {
        if (g_logs[i].level == level && strcmp(g_logs[i].text, text) == 0) {
            return true;
        }
    }
    return false;
}

static bool logged_contains(int level, const char *text)
{
    unsigned i;

    for (i = 0u; i < g_log_count; ++i) {
        if (g_logs[i].level == level && strstr(g_logs[i].text, text) != NULL) {
            return true;
        }
    }
    return false;
}

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
static bool any_log_contains(const char *text)
{
    unsigned i;

    for (i = 0u; i < g_log_count; ++i) {
        if (strstr(g_logs[i].text, text) != NULL) {
            return true;
        }
    }
    return false;
}
#endif

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
    uint16_t crc = od_config_tlv_crc16(od_span_make(b->bytes, b->len));

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

static void test_parse_logging(void)
{
    struct od_config cfg;
    struct blob b;
    uint8_t body[sizeof(struct DisplayConfig)];
    char expected[LOG_TEXT_CAP];
    unsigned i;

    CASE("successful parse outcomes are logged by shared config");
    blob_start(&b, 3u);
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10u);
    blob_add(&b, 0x01u, body, (uint16_t)sizeof(struct SystemConfig));
    blob_finish(&b, true);
    logs_reset();
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
    (void)snprintf(expected, sizeof expected, "Parsing config: %u bytes", (unsigned)b.len);
    CHECK(logged_exact(OD_LOG_INFO, expected));
    CHECK(logged_exact(OD_LOG_INFO,
          "Config parsed: version=3, displays=0, leds=0, sensors=0, data_buses=0, "
          "binary_inputs=0, buzzers=0, nfc=0, flash=0"));

    CASE("advisory CRC mismatch is logged without rejecting the config");
    blob_start(&b, 1u);
    blob_finish(&b, false);
    logs_reset();
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
    CHECK(logged_contains(OD_LOG_WARN, "Config CRC mismatch: stored 0x"));

    CASE("instance cap drops are logged");
    blob_start(&b, 1u);
    for (i = 0u; i < OD_CONFIG_MAX_DISPLAYS + 1u; ++i) {
        fill_body(body, (uint16_t)sizeof(struct DisplayConfig), (uint8_t)i);
        blob_add(&b, 0x20u, body, (uint16_t)sizeof(struct DisplayConfig));
    }
    blob_finish(&b, true);
    logs_reset();
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
    CHECK(logged_exact(OD_LOG_WARN, "1 config packet(s) dropped at an instance cap"));

    CASE("unknown and malformed config outcomes are logged");
    blob_start(&b, 1u);
    blob_add(&b, 0x7Eu, body, 1u);
    blob_finish(&b, true);
    logs_reset();
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
    CHECK(logged_exact(OD_LOG_WARN, "Unknown config packet 0x7E; remainder skipped"));

    blob_start(&b, 1u);
    blob_add(&b, 0x20u, NULL, 1u);
    blob_finish(&b, true);
    logs_reset();
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_TRUNCATED);
    CHECK(logged_exact(OD_LOG_ERROR, "Config truncated: a packet exceeds the blob"));

    memset(&b, 0, sizeof b);
    b.len = 4u;
    logs_reset();
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_TOO_SHORT);
    CHECK(logged_exact(OD_LOG_ERROR, "Config too short: 4 bytes"));
}

static void test_config_dump_logging(void)
{
    struct od_config cfg;

    CASE("the complete config dump is debug-only and redacts secrets");
    od_config_reset(&cfg);
    cfg.loaded = true;

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    cfg.version = 4u;
    cfg.display_count = 1u;
    cfg.led_count = 1u;
    cfg.sensor_count = 1u;
    cfg.data_bus_count = 1u;
    cfg.binary_input_count = 1u;
    cfg.flash_config_count = 1u;
#if OD_CONFIG_WITH_TOUCH
    cfg.touch_controller_count = 1u;
#endif
#if OD_CONFIG_WITH_BUZZER
    cfg.passive_buzzer_count = 1u;
#endif
#if OD_CONFIG_WITH_NFC
    cfg.nfc_config_count = 1u;
#endif
#if OD_CONFIG_WITH_WIFI
    cfg.wifi_config_loaded = true;
    memcpy(cfg.wifi_config.ssid, "SECRET_SSID", sizeof("SECRET_SSID"));
    memcpy(cfg.wifi_config.password, "SECRET_PASSWORD", sizeof("SECRET_PASSWORD"));
    memcpy(cfg.wifi_config.server_host, "SECRET_SERVER", sizeof("SECRET_SERVER"));
#endif
    cfg.security_loaded = true;
    memcpy(cfg.security.encryption_key, "SECRET_KEY_BYTES", sizeof cfg.security.encryption_key);
#if OD_CONFIG_WITH_DATA_EXTENDED
    cfg.data_extended_loaded = true;
    memcpy(cfg.data_extended.model_name, "Model One", sizeof("Model One"));
#endif

    logs_reset();
    od_config_log_dump(&cfg);
    CHECK(logged_exact(OD_LOG_DEBUG, "=== Configuration Summary ==="));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- System Configuration ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Manufacturer Data ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Power Configuration ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Display Configurations (1) ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- LED Configurations (1) ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Sensor Configurations (1) ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Data Bus Configurations (1) ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Binary Input Configurations (1) ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Security Configuration ---"));
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Flash Configurations (1) ---"));
#if OD_CONFIG_WITH_WIFI
    CHECK(logged_contains(OD_LOG_DEBUG, "SSID / Password: 11 chars / set"));
    CHECK(!any_log_contains("SECRET_SSID"));
    CHECK(!any_log_contains("SECRET_PASSWORD"));
    CHECK(!any_log_contains("SECRET_SERVER"));
#endif
    CHECK(!any_log_contains("SECRET_KEY_BYTES"));
#if OD_CONFIG_WITH_TOUCH
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Touch Controllers (1) ---"));
#endif
#if OD_CONFIG_WITH_BUZZER
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Buzzers (1) ---"));
#endif
#if OD_CONFIG_WITH_NFC
    CHECK(logged_contains(OD_LOG_DEBUG, "--- NFC Configurations (1) ---"));
#endif
#if OD_CONFIG_WITH_DATA_EXTENDED
    CHECK(logged_contains(OD_LOG_DEBUG, "--- Data Extended ---"));
    CHECK(logged_exact(OD_LOG_DEBUG, "  model_name: Model One"));
#endif
#else
    logs_reset();
    od_config_log_dump(&cfg);
    CHECK(g_log_count == 0u);
#endif
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

    r = od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep);
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
    r = od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep);
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

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_OK);
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

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_OK);
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

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
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

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
    CHECK(cfg.security.encryption_enabled == 1u);
    CHECK(od_config_security_key_set(&cfg.security));

    CASE("a key set with the flag clear stays disabled");
    memset(&sec, 0, sizeof sec);
    sec.encryption_enabled = 0u;
    sec.encryption_key[0] = 0xA5u;
    blob_start(&b, 1u);
    blob_add(&b, 0x27, (const uint8_t *)&sec, (uint16_t)sizeof sec);
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
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

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);
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

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_TRUNCATED);
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
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_TOO_SHORT);
    CHECK(!cfg.loaded);

    CASE("an unknown id ends the walk and is reported, without failing the parse");
    blob_start(&b, 1u);
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x30);
    blob_add(&b, 0x04, body, (uint16_t)sizeof(struct PowerOption));
    blob_add(&b, 0x7E, body, 4u);     /* not a canonical id */
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x10);
    blob_add(&b, 0x01, body, (uint16_t)sizeof(struct SystemConfig));
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_OK);
    CHECK(cfg.loaded);
    CHECK(rep.unknown_id == 0x7Eu);
    CHECK(rep.stored == 1u);
    /* The packet BEHIND the unknown id was abandoned -- current fleet behaviour, preserved. */
    CHECK(cfg.system_config.device_flags == 0u);

    CASE("a NULL blob and a NULL config are refused, not dereferenced");
    CHECK(od_config_parse(&cfg, od_span_make(NULL, 100u), &rep) == OD_CFG_TLV_TOO_SHORT);
    CHECK(od_config_parse(NULL, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_TOO_SHORT);
    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), NULL) == OD_CFG_TLV_OK);   /* NULL report is fine */
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
    CHECK(od_config_apply_packet(&cfg, 0x21, od_span_make(body, (uint16_t)sizeof(struct LedConfig)))
          == OD_CONFIG_APPLY_STORED);
    CHECK(cfg.led_count == 1u);

    CASE("a body shorter than the struct is refused rather than over-read");
    od_config_reset(&cfg);
    CHECK(od_config_apply_packet(&cfg, 0x21, od_span_make(body, 2u)) == OD_CONFIG_APPLY_SHORT_BODY);
    CHECK(cfg.led_count == 0u);
    CHECK(od_config_apply_packet(&cfg, 0x01, od_span_make(body, 2u)) == OD_CONFIG_APPLY_SHORT_BODY);

    CASE("an id this table does not know is reported, not stored");
    CHECK(od_config_apply_packet(&cfg, 0x7E, od_span_make(body, 64u)) == OD_CONFIG_APPLY_UNKNOWN_ID);

    CASE("NULL arguments are refused");
    CHECK(od_config_apply_packet(NULL, 0x21, od_span_make(body, 64u)) == OD_CONFIG_APPLY_SHORT_BODY);
    CHECK(od_config_apply_packet(&cfg, 0x21, od_span_make(NULL, 64u)) == OD_CONFIG_APPLY_SHORT_BODY);

    CASE("reset clears every count");
    od_config_reset(&cfg);
    for (i = 0; i < 3u; ++i) {
        fill_body(body, (uint16_t)sizeof(struct SensorData), (uint8_t)i);
        (void)od_config_apply_packet(&cfg, 0x23, od_span_make(body, (uint16_t)sizeof(struct SensorData)));
    }
    CHECK(cfg.sensor_count == 3u);
    od_config_reset(&cfg);
    CHECK(cfg.sensor_count == 0u);
    CHECK(!cfg.loaded);
    CHECK(!cfg.security_loaded);

    CASE("nfc_config stores like any other repeatable packet");
    od_config_reset(&cfg);
    fill_body(body, (uint16_t)sizeof(struct NfcConfig), 0x60);
#if OD_CONFIG_WITH_NFC
    CHECK(od_config_apply_packet(&cfg, 0x2A, od_span_make(body, (uint16_t)sizeof(struct NfcConfig)))
          == OD_CONFIG_APPLY_STORED);
    CHECK(cfg.nfc_config_count == 1u);
#else
    CHECK(od_config_apply_packet(&cfg, 0x2A, od_span_make(body, (uint16_t)sizeof(struct NfcConfig)))
          == OD_CONFIG_APPLY_NOT_BUILT);
#endif
}

/* A target parses and stores every canonical packet whether or not it can act on it (settled
 * 2026-08-13). The assertion that matters is not that NFC is stored -- it is that the packets
 * BEHIND it survive, because the failure this replaced was a config silently losing its
 * flash_config and data_extended to a device with no NFC hardware. */
static void test_nfc_does_not_end_the_walk(void)
{
    struct od_config cfg;
    struct od_config_report rep;
    struct blob b;
    uint8_t body[512];

    CASE("an nfc_config packet does not abandon the rest of the blob");
    blob_start(&b, 1u);
    fill_body(body, (uint16_t)sizeof(struct NfcConfig), 0x60);
    blob_add(&b, 0x2A, body, (uint16_t)sizeof(struct NfcConfig));
    fill_body(body, (uint16_t)sizeof(struct FlashConfig), 0x70);
    blob_add(&b, 0x2B, body, (uint16_t)sizeof(struct FlashConfig));
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x80);
    blob_add(&b, 0x01, body, (uint16_t)sizeof(struct SystemConfig));
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_OK);
    CHECK(cfg.loaded);
    CHECK(rep.unknown_id == 0u);            /* 0x2A is known; nothing ended the walk */
    CHECK(cfg.flash_config_count == 1u);    /* the packet behind NFC was applied */
    fill_body(body, (uint16_t)sizeof(struct SystemConfig), 0x80);
    CHECK(memcmp(&cfg.system_config, body, sizeof(struct SystemConfig)) == 0);

#if OD_CONFIG_WITH_NFC
    CHECK(rep.stored == 3u);
    CHECK(cfg.nfc_config_count == 1u);
    fill_body(body, (uint16_t)sizeof(struct NfcConfig), 0x60);
    CHECK(memcmp(&cfg.nfc_configs[0], body, sizeof(struct NfcConfig)) == 0);
#else
    /* A build without NFC storage still walks past the packet -- it is counted, not stored, and
     * the packets behind it are unaffected. That is the whole point of the distinction. */
    CHECK(rep.stored == 2u);
    CHECK(rep.dropped_not_built == 1u);
#endif

    CASE("the nfc cap skips like every other, without ending the walk");
    blob_start(&b, 1u);
    fill_body(body, (uint16_t)sizeof(struct NfcConfig), 0x60);
    blob_add(&b, 0x2A, body, (uint16_t)sizeof(struct NfcConfig));
    blob_add(&b, 0x2A, body, (uint16_t)sizeof(struct NfcConfig));
    blob_add(&b, 0x2A, body, (uint16_t)sizeof(struct NfcConfig));
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x90);
    blob_add(&b, 0x04, body, (uint16_t)sizeof(struct PowerOption));
    blob_finish(&b, true);

    CHECK(od_config_parse(&cfg, od_span_make(b.bytes, b.len), &rep) == OD_CFG_TLV_OK);
    fill_body(body, (uint16_t)sizeof(struct PowerOption), 0x90);
    CHECK(memcmp(&cfg.power_option, body, sizeof(struct PowerOption)) == 0);
#if OD_CONFIG_WITH_NFC
    CHECK(cfg.nfc_config_count == OD_CONFIG_MAX_NFC);
    CHECK(rep.dropped_full == 1u);
#else
    CHECK(rep.dropped_not_built == 3u);
#endif
}

/* od_config_data_bus: the key is instance_number, and ambiguity is refused.
 *
 * The cases that matter are the ones where index and instance DISAGREE, because agreement is the
 * common configuration and is why indexing survived this long. */
static void test_data_bus_lookup(void)
{
    static struct od_config cfg;

    CASE("in-order records: index and instance agree");
    od_config_reset(&cfg);
    cfg.data_bus_count = 2u;
    cfg.data_buses[0].instance_number = 0u;
    cfg.data_buses[0].pin_1 = 10u;
    cfg.data_buses[1].instance_number = 1u;
    cfg.data_buses[1].pin_1 = 20u;
    CHECK(od_config_data_bus(&cfg, 0u) == &cfg.data_buses[0]);
    CHECK(od_config_data_bus(&cfg, 1u) == &cfg.data_buses[1]);

    CASE("out-of-order records: indexing would return the wrong bus");
    od_config_reset(&cfg);
    cfg.data_bus_count = 2u;
    cfg.data_buses[0].instance_number = 1u;   /* instance 1 arrived first */
    cfg.data_buses[0].pin_1 = 20u;
    cfg.data_buses[1].instance_number = 0u;
    cfg.data_buses[1].pin_1 = 10u;
    CHECK(od_config_data_bus(&cfg, 0u) == &cfg.data_buses[1]);   /* NOT data_buses[0] */
    CHECK(od_config_data_bus(&cfg, 1u) == &cfg.data_buses[0]);

    CASE("sparse records: an absent instance is refused, not clamped");
    od_config_reset(&cfg);
    cfg.data_bus_count = 2u;
    cfg.data_buses[0].instance_number = 1u;
    cfg.data_buses[0].pin_1 = 11u;
    cfg.data_buses[1].instance_number = 3u;
    cfg.data_buses[1].pin_1 = 33u;
    CHECK(od_config_data_bus(&cfg, 0u) == NULL);
    CHECK(od_config_data_bus(&cfg, 2u) == NULL);
    /* The exact record, not merely "a" record: `!= NULL` would accept what indexing returns. */
    CHECK(od_config_data_bus(&cfg, 1u) == &cfg.data_buses[0]);
    CHECK(od_config_data_bus(&cfg, 3u) == &cfg.data_buses[1]);

    CASE("duplicate instance is ambiguous: no bus, never the first by packet order");
    od_config_reset(&cfg);
    cfg.data_bus_count = 3u;
    cfg.data_buses[0].instance_number = 2u;
    cfg.data_buses[0].pin_1 = 10u;
    cfg.data_buses[1].instance_number = 5u;
    cfg.data_buses[2].instance_number = 2u;   /* declared twice */
    cfg.data_buses[2].pin_1 = 99u;
    CHECK(od_config_data_bus(&cfg, 2u) == NULL);
    CHECK(od_config_data_bus(&cfg, 5u) == &cfg.data_buses[1]);   /* the unambiguous one resolves */

    CASE("empty config and a null config resolve to nothing");
    od_config_reset(&cfg);
    CHECK(od_config_data_bus(&cfg, 0u) == NULL);
    CHECK(od_config_data_bus(NULL, 0u) == NULL);

    CASE("0xFF is taken literally, not treated as a sentinel here");
    /* Refusing the sentinel is the CONSUMER's rule -- a resolver that invented a default for it
     * is how DIVERGENCE_MATRIX 13 happened. A record genuinely numbered 0xFF resolves. */
    od_config_reset(&cfg);
    cfg.data_bus_count = 2u;
    cfg.data_buses[0].instance_number = 0u;
    cfg.data_buses[0].pin_1 = 10u;
    cfg.data_buses[1].instance_number = 0xFFu;
    cfg.data_buses[1].pin_1 = 77u;
    CHECK(od_config_data_bus(&cfg, 0xFFu) == &cfg.data_buses[1]);

    CASE("a count beyond the array is bounded by the array");
    od_config_reset(&cfg);
    cfg.data_bus_count = 200u;   /* a corrupted count must not walk off the end */
    cfg.data_buses[0].instance_number = 4u;
    CHECK(od_config_data_bus(&cfg, 4u) == &cfg.data_buses[0]);
    CHECK(od_config_data_bus(&cfg, 9u) == NULL);
    /* Every reachable slot past the real records is zeroed, so instance 0 matches all of them and
     * must read as ambiguous rather than as slot 0. An implementation that trusted the count
     * would instead run off the array. */
    CHECK(od_config_data_bus(&cfg, 0u) == NULL);
}

int main(void)
{
    test_parse_logging();
    test_config_dump_logging();
    test_single_instance_packets();
    test_data_bus_lookup();
    test_instance_caps();
    test_security_zero_key();
#if OD_CONFIG_WITH_DATA_EXTENDED
    test_data_extended_termination();
#endif
    test_hostile_blobs();
    test_apply_directly();
    test_nfc_does_not_end_the_walk();

    printf("config: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
