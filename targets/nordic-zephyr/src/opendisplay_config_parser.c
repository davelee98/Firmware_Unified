/*
 * Config load for targets/nordic-zephyr.
 *
 * THE WALK AND THE STORE ARE BOTH SHARED NOW. What was here was a 530-line switch that
 * re-derived, per packet type, the offset arithmetic, the "does this fit" comparison against
 * configLen - 2, the instance caps, and its own CRC-16 and packet-size table. All of that is
 * shared/core/od_config_tlv.c (the walk) and shared/core/od_config.c (the store); what is left
 * is this target's own: reading the blob out of storage, and saying what happened.
 *
 * Two things this file used to do are deliberately NOT carried across:
 *
 *   rescan_security_packet(). It scanned the raw blob for an 0x27 the walk had stepped past, and
 *   existed because an unknown id ends the walk: a blob carrying 0x2C on a build that did not
 *   know 0x2C dropped the security packet behind it. The shared size table knows every canonical
 *   id, so the case it recovered from cannot arise for a canonical packet; keeping the scan would
 *   carry a workaround into the one path a client can write pre-auth, and would leave the next
 *   missing id silently survivable. See shared/core/od_config.h.
 *
 *   The per-packet "known-unparsed, skipping N bytes" arm. Every id in the table is dispatched
 *   and stored now, so there is no such thing as known-but-unparsed here.
 */

#include "opendisplay_config_parser.h"
#include "od_log.h"
#include "opendisplay_constants.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_device_flags.h"
#include <stdio.h>
#include <string.h>

/* The config parsed last, for the two security accessors below. A pointer rather than a copy:
 * security lives inside the aggregate so that nothing can read an encryption_enabled the
 * zero-key rule has not been applied to, and a second copy here would be exactly that hole. */
static const struct od_config *s_parsed;

/* Returned before any parse has run. Non-NULL so the callers that check for NULL keep their
 * meaning: no config means no encryption, not "unknown". */
static const struct SecurityConfig s_no_security;

const struct SecurityConfig *od_get_parsed_security(void)
{
	return (s_parsed != NULL) ? &s_parsed->security : &s_no_security;
}

bool od_security_key_set(void)
{
	return od_config_security_key_set(od_get_parsed_security());
}

static void log_parse_result(const struct od_config *cfg, const struct od_config_report *report)
{
	if (report->unknown_id != 0u) {
		od_log_info("Unknown pkt 0x%02X, remainder of config skipped", report->unknown_id);
	}
	if (report->dropped_full != 0u) {
		od_log_info("%u pkt(s) dropped at an instance cap", (unsigned)report->dropped_full);
	}
	if (report->dropped_not_built != 0u) {
		od_log_info("%u pkt(s) for subsystems not built in",
			    (unsigned)report->dropped_not_built);
	}
	if (report->crc_checked && report->crc_stored != report->crc_computed) {
		od_log_info("CRC mismatch: 0x%04X vs 0x%04X", report->crc_stored,
			    report->crc_computed);
	}

	if ((cfg->system_config.device_flags & DEVICE_FLAG_CHANNEL_SOUNDING) != 0u) {
		od_log_info("system_config: CHANNEL_SOUNDING enabled");
	}
	/* Panel bring-up leans on these four lines more than on anything else this file logs: a
	 * wrong pin or a wrong IC shows up here and nowhere else until the display stays blank. */
	for (uint8_t i = 0; i < cfg->display_count; i++) {
		const struct DisplayConfig *d = &cfg->displays[i];

		od_log_info("Display %u: ic=0x%04X %dx%d", (unsigned)i, d->panel_ic_type,
			    d->pixel_width, d->pixel_height);
		od_log_info("Display %u: RST=%d BUSY=%d DC=%d", (unsigned)i, d->reset_pin,
			    d->busy_pin, d->dc_pin);
		od_log_info("Display %u: CS=%d DATA=%d CLK=%d", (unsigned)i, d->cs_pin,
			    d->data_pin, d->clk_pin);
		od_log_info("Display %u: color=%d modes=0x%02X", (unsigned)i, d->color_scheme,
			    d->transmission_modes);
	}
	if (cfg->security_loaded) {
		od_log_info("Security: enabled=%d, flags=0x%02X, reset_pin=%d",
			    (int)cfg->security.encryption_enabled, (unsigned)cfg->security.flags,
			    (int)cfg->security.reset_pin);
	}
	od_log_info("Config parsed: version=%d, displays=%d, leds=%d, sensors=%d, data_buses=%d, binary_inputs=%d, buzzers=%d, nfc=%d, flash=%d",
		    cfg->version, cfg->display_count, cfg->led_count, cfg->sensor_count,
		    cfg->data_bus_count, cfg->binary_input_count, cfg->passive_buzzer_count,
		    cfg->nfc_config_count, cfg->flash_config_count);
}

bool parseConfigBytes(uint8_t *configData, uint32_t configLen, struct od_config *globalConfig)
{
	struct od_config_report report;
	enum od_config_tlv_result walk;

	if (globalConfig == NULL || configData == NULL) {
		od_log_info("Invalid parameters for parseConfigBytes");
		return false;
	}

	od_log_info("Parsing config: %u bytes", (unsigned)configLen);

	/* Resets, walks, stores, normalises, and computes the advisory CRC. globalConfig->loaded is
	 * set only on a clean walk; a blob that truncates half-way keeps the packets that preceded
	 * the truncation, so `loaded` is what callers must read, not the counts. */
	walk = od_config_parse(globalConfig, od_span_make(configData, configLen), &report);
	s_parsed = globalConfig;

	if (walk == OD_CFG_TLV_TOO_SHORT) {
		od_log_info("Config too short: %u bytes", (unsigned)configLen);
		return false;
	}
	if (walk != OD_CFG_TLV_OK) {
		/* Reported per packet type before the walk was shared ("system_config: need %zu,
		 * have %u"); the walk cannot name the type, so this is the same fact, once. */
		od_log_info("Config truncated: a packet claims more data than the blob holds");
		return false;
	}

	log_parse_result(globalConfig, &report);
	return true;
}

bool loadGlobalConfig(struct od_config *globalConfig)
{
	static uint8_t configData[MAX_CONFIG_SIZE];
	uint32_t configLen = MAX_CONFIG_SIZE;

	if (globalConfig == NULL) {
		od_log_info("Invalid parameter for loadGlobalConfig");
		return false;
	}

	od_config_reset(globalConfig);
	s_parsed = globalConfig;

	if (!initConfigStorage()) {
		od_log_info("Failed to initialize config storage");
		return false;
	}

	if (!loadConfig(configData, &configLen)) {
		od_log_info("No config found");
		return false;
	}

	return parseConfigBytes(configData, configLen, globalConfig);
}
