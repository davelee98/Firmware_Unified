#include "opendisplay_cs.h"
#include "opendisplay_device_flags.h"
#include "opendisplay_structs.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

#if defined(CONFIG_BT_CHANNEL_SOUNDING)
#include <zephyr/bluetooth/cs.h>
#include <bluetooth/services/ras.h>
#include <zephyr/sys/byteorder.h>
#endif

LOG_MODULE_REGISTER(opendisplay_cs, LOG_LEVEL_INF);

bool opendisplay_cs_config_enabled(const struct GlobalConfig *cfg)
{
#if !defined(CONFIG_BT_CHANNEL_SOUNDING)
	ARG_UNUSED(cfg);
	return false;
#else
	if (cfg == NULL || !cfg->loaded) {
		return false;
	}
	return (cfg->system_config.device_flags & DEVICE_FLAG_CHANNEL_SOUNDING) != 0u;
#endif
}

unsigned opendisplay_cs_scan_response_count(const struct GlobalConfig *cfg)
{
	return opendisplay_cs_config_enabled(cfg) ? 1u : 0u;
}

void opendisplay_cs_fill_scan_response(const struct GlobalConfig *cfg, struct bt_data *out,
				       unsigned max_entries, unsigned *count_out)
{
	*count_out = 0;
#if defined(CONFIG_BT_CHANNEL_SOUNDING)
	static const uint8_t ranging_uuid[] = { BT_UUID_16_ENCODE(BT_UUID_RANGING_SERVICE_VAL) };

	if (!opendisplay_cs_config_enabled(cfg) || max_entries == 0u) {
		return;
	}
	out[0].type = BT_DATA_UUID16_ALL;
	out[0].data_len = sizeof(ranging_uuid);
	out[0].data = ranging_uuid;
	*count_out = 1;
#else
	ARG_UNUSED(cfg);
	ARG_UNUSED(out);
	ARG_UNUSED(max_entries);
#endif
}

#if defined(CONFIG_BT_CHANNEL_SOUNDING)

static struct bt_conn *s_cs_conn;
static bool s_cs_defaults_applied;
static bool s_cs_procedure_params_set;

static int cs_apply_default_settings(struct bt_conn *conn)
{
	const struct bt_le_cs_set_default_settings_param default_settings = {
		.enable_initiator_role = false,
		.enable_reflector_role = true,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};
	int err;

	if (conn == NULL || s_cs_defaults_applied) {
		return 0;
	}

	err = bt_le_cs_set_default_settings(conn, &default_settings);
	if (err != 0) {
		LOG_ERR("CS default settings failed (%d)", err);
		return err;
	}

	s_cs_defaults_applied = true;
	LOG_INF("CS reflector defaults applied");
	return 0;
}

static int cs_apply_procedure_parameters(struct bt_conn *conn, uint8_t config_id)
{
	const struct bt_le_cs_set_procedure_parameters_param procedure_params = {
		.config_id = config_id,
		.max_procedure_len = 1000,
		.min_procedure_interval = 1,
		.max_procedure_interval = 100,
		.max_procedure_count = 0,
		.min_subevent_len = 10000,
		.max_subevent_len = 75000,
		.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
		.phy = BT_LE_CS_PROCEDURE_PHY_2M,
		.tx_power_delta = 0x80,
		.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
		.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
		.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
	};
	int err;

	if (conn == NULL || s_cs_procedure_params_set) {
		return 0;
	}

	err = bt_le_cs_set_procedure_parameters(conn, &procedure_params);
	if (err != 0) {
		LOG_ERR("CS procedure parameters failed (%d)", err);
		return err;
	}

	s_cs_procedure_params_set = true;
	LOG_INF("CS procedure parameters applied (config %u)", config_id);
	return 0;
}

static void cs_remote_capabilities_cb(struct bt_conn *conn, uint8_t status,
				      struct bt_conn_le_cs_capabilities *params)
{
	ARG_UNUSED(params);

	if (status == BT_HCI_ERR_SUCCESS) {
		LOG_INF("CS capability exchange completed");
	} else {
		LOG_WRN("CS capability exchange failed (HCI 0x%02x)", status);
	}
}

static void cs_config_create_cb(struct bt_conn *conn, uint8_t status,
				struct bt_conn_le_cs_config *config)
{
	if (status == BT_HCI_ERR_SUCCESS) {
		LOG_INF("CS config creation complete (id %u)", config->id);
		(void)cs_apply_procedure_parameters(conn, config->id);
	} else {
		LOG_WRN("CS config creation failed (HCI 0x%02x)", status);
	}
}

static void cs_security_enable_cb(struct bt_conn *conn, uint8_t status)
{
	ARG_UNUSED(conn);

	if (status == BT_HCI_ERR_SUCCESS) {
		LOG_INF("CS security enabled");
	} else {
		LOG_WRN("CS security enable failed (HCI 0x%02x)", status);
	}
}

static void cs_procedure_enable_cb(struct bt_conn *conn, uint8_t status,
				   struct bt_conn_le_cs_procedure_enable_complete *params)
{
	ARG_UNUSED(conn);

	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("CS procedure enable failed (HCI 0x%02x)", status);
		return;
	}
	if (params->state == 1) {
		LOG_INF("CS procedures enabled (config %u)", params->config_id);
	} else {
		LOG_INF("CS procedures disabled");
	}
}

BT_CONN_CB_DEFINE(cs_conn_cb) = {
	.le_cs_read_remote_capabilities_complete = cs_remote_capabilities_cb,
	.le_cs_config_complete = cs_config_create_cb,
	.le_cs_security_enable_complete = cs_security_enable_cb,
	.le_cs_procedure_enable_complete = cs_procedure_enable_cb,
};

#endif /* CONFIG_BT_CHANNEL_SOUNDING */

void opendisplay_cs_on_connected(struct bt_conn *conn)
{
#if defined(CONFIG_BT_CHANNEL_SOUNDING)
	if (s_cs_conn != NULL) {
		bt_conn_unref(s_cs_conn);
		s_cs_conn = NULL;
	}
	s_cs_defaults_applied = false;
	s_cs_procedure_params_set = false;
	s_cs_conn = bt_conn_ref(conn);
	/* One fast HCI call so ranging can start when the peer asks; no wait for config. */
	(void)cs_apply_default_settings(conn);
#else
	ARG_UNUSED(conn);
#endif
}

void opendisplay_cs_on_disconnected(struct bt_conn *conn)
{
#if defined(CONFIG_BT_CHANNEL_SOUNDING)
	ARG_UNUSED(conn);
	if (s_cs_conn != NULL) {
		bt_conn_unref(s_cs_conn);
		s_cs_conn = NULL;
	}
	s_cs_defaults_applied = false;
	s_cs_procedure_params_set = false;
#else
	ARG_UNUSED(conn);
#endif
}
