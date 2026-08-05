#include "opendisplay_nfc.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "opendisplay_structs.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_NFC_T2T_NRFXLIB)
#include <nfc_t2t_lib.h>
#endif

/* Match pipe chunk staging; T2T allows up to NFC_T2T_MAX_PAYLOAD_SIZE (988). */
#define OD_NFC_NDEF_MAX 512u

#if defined(CONFIG_NFC_T2T_NRFXLIB)

static bool s_nfc_enabled;
static bool s_nfc_emulating;
static uint8_t s_ndef[OD_NFC_NDEF_MAX];
static uint16_t s_ndef_len;
static uint8_t s_adv_byte_index = 0xFFu;
static uint8_t s_field_state;
static uint8_t s_field_scan_counter;
static uint32_t s_field_last_ms;
static uint8_t s_field_pending;
static bool s_field_pending_valid;

#define OD_NFC_FIELD_DEBOUNCE_MS 300u

static void nfc_publish_field_msd(void)
{
	uint8_t packed;

	if (s_adv_byte_index > 10u) {
		return;
	}
	packed = (uint8_t)((s_field_state & 0x01u) |
			   (((s_field_scan_counter & 0x7Fu) << 1) & 0xFEu));
	/* Only touch the field byte — do not call update_msd() (that re-polls
	 * nPM1300 and can cache a failed battery read for 30s). */
	opendisplay_ble_set_dynamic_byte(s_adv_byte_index, packed);
}

static void nfc_apply_field_state(uint8_t logical_on)
{
	if (logical_on == s_field_state) {
		return;
	}
	if (logical_on != 0u) {
		s_field_scan_counter = (uint8_t)((s_field_scan_counter + 1u) & 0x7Fu);
		s_field_state = 1u;
		nfc_publish_field_msd();
		opendisplay_ble_update_msd(true);
		opendisplay_ble_boost_advertising();
	} else {
		s_field_state = 0u;
		nfc_publish_field_msd();
		opendisplay_ble_update_msd(true);
	}
}

static void nfc_callback(void *context, nfc_t2t_event_t event, const uint8_t *data,
			 size_t data_length)
{
	uint32_t now;

	ARG_UNUSED(context);
	ARG_UNUSED(data);
	ARG_UNUSED(data_length);

	now = k_uptime_get_32();
	switch (event) {
	case NFC_T2T_EVENT_FIELD_ON:
	case NFC_T2T_EVENT_FIELD_OFF: {
		uint8_t want = (event == NFC_T2T_EVENT_FIELD_ON) ? 1u : 0u;

		/* Debounce: NFC without a stable antenna can chatter and
		 * restart advertising via boost on every edge. */
		if ((now - s_field_last_ms) < OD_NFC_FIELD_DEBOUNCE_MS) {
			s_field_pending = want;
			s_field_pending_valid = true;
			return;
		}
		s_field_last_ms = now;
		s_field_pending_valid = false;
		nfc_apply_field_state(want);
		break;
	}
	default:
		break;
	}
}

void opendisplay_nfc_process(void)
{
	uint32_t now;

	if (!s_field_pending_valid) {
		return;
	}
	now = k_uptime_get_32();
	if ((now - s_field_last_ms) < OD_NFC_FIELD_DEBOUNCE_MS) {
		return;
	}
	s_field_last_ms = now;
	s_field_pending_valid = false;
	nfc_apply_field_state(s_field_pending);
}

static bool nfc_apply_payload(void)
{
	int err;

	if (s_ndef_len == 0u) {
		/* Empty NDEF record so the tag is readable before first write. */
		s_ndef[0] = 0xD0u;
		s_ndef[1] = 0x00u;
		s_ndef[2] = 0x00u;
		s_ndef_len = 3u;
	}

	if (s_nfc_emulating) {
		(void)nfc_t2t_emulation_stop();
		s_nfc_emulating = false;
	}

	err = nfc_t2t_payload_set(s_ndef, s_ndef_len);
	if (err != 0) {
		printf("[OD][NFC] payload_set failed: %d\r\n", err);
		return false;
	}

	err = nfc_t2t_emulation_start();
	if (err != 0) {
		printf("[OD][NFC] emulation_start failed: %d\r\n", err);
		return false;
	}

	s_nfc_emulating = true;
	return true;
}

static void nfc_stop(void)
{
	if (s_nfc_emulating) {
		(void)nfc_t2t_emulation_stop();
		s_nfc_emulating = false;
	}
	s_nfc_enabled = false;
	s_field_state = 0u;
	s_adv_byte_index = 0xFFu;
}

static bool nfc_ic_is_soc(uint8_t ic_type)
{
	if (ic_type == OD_NFC_IC_SOC_NFCT) {
		return true;
	}
	/* LM20: auto resolves to SoC NFCT (no external TNB132M). */
	if (ic_type == OD_NFC_IC_AUTO) {
#if defined(NRF54_BOARD_LM20)
		return true;
#else
		return false;
#endif
	}
	return false;
}

static bool nfc_parse_ndef(uint8_t *type_out, uint8_t *out, uint16_t *io_len, uint16_t out_max)
{
	const uint8_t *data = s_ndef;
	uint16_t ln = s_ndef_len;
	bool sr;
	uint8_t tlen;
	uint8_t plen;
	uint16_t off;
	uint16_t copy_len = 0u;

	if (type_out == NULL || out == NULL || io_len == NULL || ln < 3u) {
		return false;
	}

	sr = (data[0] & 0x10u) != 0u;
	if (!sr) {
		return false;
	}
	if ((data[0] & 0x08u) != 0u) {
		if (ln > out_max) {
			*io_len = out_max;
		} else {
			*io_len = ln;
		}
		*type_out = OD_NFC_REC_RAW_NDEF;
		memcpy(out, data, *io_len);
		return true;
	}

	tlen = data[1];
	plen = data[2];
	off = 3u;
	if (((uint32_t)off + (uint32_t)tlen + (uint32_t)plen) > ln) {
		return false;
	}

	switch (data[0] & 7u) {
	case 2u: {
		uint16_t out_pack;

		*type_out = OD_NFC_REC_MIME;
		if (plen > 255u || tlen == 0u) {
			return false;
		}
		out_pack = (uint16_t)(1u + tlen + plen);
		if (out_pack > out_max) {
			return false;
		}
		out[0] = tlen;
		memcpy(&out[1], &data[off], tlen);
		memcpy(&out[1u + tlen], &data[(uint16_t)(off + tlen)], plen);
		*io_len = out_pack;
		return true;
	}
	case 1u:
		break;
	default:
		if (ln > out_max) {
			*io_len = out_max;
		} else {
			*io_len = ln;
		}
		*type_out = OD_NFC_REC_RAW_NDEF;
		memcpy(out, data, *io_len);
		return true;
	}

	if (tlen == 1u && data[off] == 'T') {
		uint8_t status;
		uint8_t lang_len;

		*type_out = OD_NFC_REC_TEXT;
		off = (uint16_t)(off + 1u);
		if (plen < 1u) {
			return false;
		}
		status = data[off];
		lang_len = status & 0x3Fu;
		if ((uint16_t)(1u + lang_len) > plen) {
			return false;
		}
		off = (uint16_t)(off + 1u + lang_len);
		copy_len = (uint16_t)(plen - 1u - lang_len);
	} else if (tlen == 1u && data[off] == 'U') {
		uint8_t uri_prefix;

		*type_out = OD_NFC_REC_URI;
		off = (uint16_t)(off + 1u);
		if (plen < 1u) {
			return false;
		}
		uri_prefix = data[off];
		if (uri_prefix != 0x00u) {
			return false;
		}
		off = (uint16_t)(off + 1u);
		copy_len = (uint16_t)(plen - 1u);
	} else {
		uint16_t raw_len;

		*type_out = OD_NFC_REC_WELL_KNOWN_RAW;
		raw_len = (uint16_t)(1u + tlen + plen);
		if (raw_len > out_max) {
			raw_len = out_max;
		}
		if (raw_len == 0u) {
			*io_len = 0u;
			return true;
		}
		out[0] = tlen;
		copy_len = (uint16_t)(raw_len - 1u);
		memcpy(&out[1], &data[off], copy_len);
		*io_len = raw_len;
		return true;
	}

	if (copy_len > out_max) {
		copy_len = out_max;
	}
	memcpy(out, &data[off], copy_len);
	*io_len = copy_len;
	return true;
}

static bool nfc_encode_ndef(uint8_t rec_type, const uint8_t *data, uint16_t data_len)
{
	uint16_t record_len;
	uint16_t payload_len;

	if (data == NULL || data_len == 0u) {
		return false;
	}
	memset(s_ndef, 0, sizeof(s_ndef));

	if (rec_type == OD_NFC_REC_TEXT) {
		payload_len = (uint16_t)(1u + 2u + data_len);
		if (payload_len > 255u || payload_len > (uint16_t)(sizeof(s_ndef) - 4u)) {
			return false;
		}
		record_len = (uint16_t)(4u + payload_len);
		s_ndef[0] = 0xD1u;
		s_ndef[1] = 0x01u;
		s_ndef[2] = (uint8_t)payload_len;
		s_ndef[3] = 0x54u;
		s_ndef[4] = 0x02u;
		s_ndef[5] = (uint8_t)'e';
		s_ndef[6] = (uint8_t)'n';
		memcpy(&s_ndef[7], data, data_len);
	} else if (rec_type == OD_NFC_REC_URI) {
		payload_len = (uint16_t)(1u + data_len);
		if (payload_len > 255u || payload_len > (uint16_t)(sizeof(s_ndef) - 4u)) {
			return false;
		}
		record_len = (uint16_t)(4u + payload_len);
		s_ndef[0] = 0xD1u;
		s_ndef[1] = 0x01u;
		s_ndef[2] = (uint8_t)payload_len;
		s_ndef[3] = 0x55u;
		s_ndef[4] = 0x00u;
		memcpy(&s_ndef[5], data, data_len);
	} else if (rec_type == OD_NFC_REC_WELL_KNOWN_RAW) {
		uint8_t type_len;
		uint16_t raw_payload_len;

		if (data_len < 2u) {
			return false;
		}
		type_len = data[0];
		if (type_len == 0u || (uint16_t)(1u + type_len) > data_len) {
			return false;
		}
		raw_payload_len = (uint16_t)(data_len - 1u - type_len);
		if (raw_payload_len > 255u) {
			return false;
		}
		record_len = (uint16_t)(3u + type_len + raw_payload_len);
		if (record_len > sizeof(s_ndef)) {
			return false;
		}
		s_ndef[0] = 0xD1u;
		s_ndef[1] = type_len;
		s_ndef[2] = (uint8_t)raw_payload_len;
		memcpy(&s_ndef[3], &data[1], type_len);
		if (raw_payload_len > 0u) {
			memcpy(&s_ndef[3u + type_len], &data[1u + type_len], raw_payload_len);
		}
	} else if (rec_type == OD_NFC_REC_MIME) {
		uint8_t mime_tl;
		uint16_t body_len;

		if (data_len < 3u) {
			return false;
		}
		mime_tl = data[0];
		if (mime_tl == 0u || (uint16_t)(1u + mime_tl) > data_len) {
			return false;
		}
		body_len = (uint16_t)(data_len - 1u - mime_tl);
		if (body_len > 255u) {
			return false;
		}
		record_len = (uint16_t)(3u + mime_tl + body_len);
		if (record_len > sizeof(s_ndef)) {
			return false;
		}
		s_ndef[0] = 0xD2u;
		s_ndef[1] = mime_tl;
		s_ndef[2] = (uint8_t)body_len;
		memcpy(&s_ndef[3], &data[1], mime_tl);
		if (body_len > 0u) {
			memcpy(&s_ndef[3u + mime_tl], &data[1u + mime_tl], body_len);
		}
	} else if (rec_type == OD_NFC_REC_RAW_NDEF) {
		record_len = data_len;
		if (record_len == 0u || record_len > sizeof(s_ndef)) {
			return false;
		}
		memcpy(s_ndef, data, record_len);
	} else {
		return false;
	}

	s_ndef_len = record_len;
	return true;
}

void opendisplay_nfc_apply_config(const struct GlobalConfig *cfg)
{
	const struct NfcConfig *nfc_cfg = NULL;
	uint8_t i;
	int err;
	static bool s_t2t_setup_done;

	if (cfg == NULL || !cfg->loaded || cfg->nfc_config_count == 0u) {
		nfc_stop();
		printf("[OD][NFC] no nfc_config (0x2A); SoC NFCT idle\r\n");
		return;
	}

	for (i = 0u; i < cfg->nfc_config_count; i++) {
		if ((cfg->nfc_configs[i].flags & 0x01u) != 0u) {
			nfc_cfg = &cfg->nfc_configs[i];
			break;
		}
	}
	if (nfc_cfg == NULL) {
		nfc_stop();
		printf("[OD][NFC] configs present but none enabled\r\n");
		return;
	}
	if (!nfc_ic_is_soc(nfc_cfg->nfc_ic_type)) {
		nfc_stop();
		printf("[OD][NFC] unsupported nfc_ic_type=%u (need auto/soc_nfct)\r\n",
		       (unsigned)nfc_cfg->nfc_ic_type);
		return;
	}

	s_adv_byte_index = nfc_cfg->adv_button_byte_index;
	s_field_state = 0u;
	s_field_scan_counter = 0u;
	nfc_publish_field_msd();

	if (!s_t2t_setup_done) {
		err = nfc_t2t_setup(nfc_callback, NULL);
		if (err != 0) {
			printf("[OD][NFC] t2t_setup failed: %d\r\n", err);
			nfc_stop();
			return;
		}
		s_t2t_setup_done = true;
	}

	s_nfc_enabled = true;
	if (!nfc_apply_payload()) {
		s_nfc_enabled = false;
		return;
	}

	printf("[OD][NFC] SoC NFCT T2T active (adv_byte=%u)\r\n",
	       (unsigned)s_adv_byte_index);
}

bool opendisplay_ble_nfc_read(uint8_t *type_out, uint8_t *data_out, uint16_t *data_len_io,
			      uint16_t max_len)
{
	if (!s_nfc_enabled) {
		return false;
	}
	return nfc_parse_ndef(type_out, data_out, data_len_io, max_len);
}

bool opendisplay_ble_nfc_write(uint8_t type, const uint8_t *data, uint16_t data_len)
{
	if (!s_nfc_enabled) {
		return false;
	}
	if (!nfc_encode_ndef(type, data, data_len)) {
		return false;
	}
	return nfc_apply_payload();
}

#else /* !CONFIG_NFC_T2T_NRFXLIB */

void opendisplay_nfc_apply_config(const struct GlobalConfig *cfg)
{
	ARG_UNUSED(cfg);
}

void opendisplay_nfc_process(void)
{
}

bool opendisplay_ble_nfc_read(uint8_t *type_out, uint8_t *data_out, uint16_t *data_len_io,
			      uint16_t max_len)
{
	ARG_UNUSED(type_out);
	ARG_UNUSED(data_out);
	ARG_UNUSED(data_len_io);
	ARG_UNUSED(max_len);
	return false;
}

bool opendisplay_ble_nfc_write(uint8_t type, const uint8_t *data, uint16_t data_len)
{
	ARG_UNUSED(type);
	ARG_UNUSED(data);
	ARG_UNUSED(data_len);
	return false;
}

#endif /* CONFIG_NFC_T2T_NRFXLIB */
