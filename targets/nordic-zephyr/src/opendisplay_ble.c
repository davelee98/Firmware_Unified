#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_cs.h"
#include "opendisplay_constants.h"
#include "opendisplay_display.h"
#include "opendisplay_button.h"
#include "opendisplay_led.h"
#include "opendisplay_touch.h"
#include "opendisplay_buzzer.h"
#include "opendisplay_pipe.h"
#include "factory_config.h"
#include "opendisplay_battery.h"
#include "opendisplay_sensor_sht40.h"
#include "opendisplay_sensor_bq27220.h"
#include "opendisplay_sensor_npm1300.h"
#include "opendisplay_nfc.h"
#include "board_nrf54.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#if defined(CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION)
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#endif
#include <errno.h>

#define OPENDISPLAY_COMPANY_ID 0x2446u
#define MSD_PAYLOAD_LEN        16u
#define OD_NAME_PREFIX         "OD"
#ifndef OD_FW_VERSION
#define OD_FW_VERSION ""
#endif

static const char *fw_build_version_string(void)
{
	const char *v = OD_FW_VERSION;

	if (v == NULL || v[0] == '\0') {
		return NULL;
	}
	/* CMake may pass a quoted string that was stringified again. */
	if (v[0] == '"') {
		v++;
	}
	if (v[0] == '\0') {
		return NULL;
	}
	return v;
}

#ifndef OD_APP_VERSION
#define OD_APP_VERSION         0x0100u
#endif

static uint8_t fw_major_from_build_version(void)
{
	const char *v = fw_build_version_string();

	if (v == NULL) {
		return 0;
	}

	while (*v == ' ' || *v == 'v' || *v == 'V') {
		v++;
	}
	if (*v < '0' || *v > '9') {
		return 0;
	}
	unsigned maj = 0U;

	while (*v >= '0' && *v <= '9') {
		maj = maj * 10U + (unsigned)(*v - '0');
		v++;
	}
	if (maj > 255U) {
		maj = 255U;
	}
	return (uint8_t)maj;
}

static uint8_t fw_minor_from_build_version(void)
{
	const char *v = fw_build_version_string();

	if (v == NULL) {
		return 0;
	}

	while (*v == ' ' || *v == 'v' || *v == 'V') {
		v++;
	}
	while (*v >= '0' && *v <= '9') {
		v++;
	}
	if (*v != '.') {
		return 0;
	}
	v++;
	if (*v < '0' || *v > '9') {
		return 0;
	}
	unsigned min = 0U;

	while (*v >= '0' && *v <= '9') {
		min = min * 10U + (unsigned)(*v - '0');
		v++;
	}
	if (min > 255U) {
		min = 255U;
	}
	return (uint8_t)min;
}

static uint8_t fw_patch_from_build_version(void)
{
	const char *v = fw_build_version_string();

	if (v == NULL) {
		return 0;
	}

	while (*v == ' ' || *v == 'v' || *v == 'V') {
		v++;
	}
	while (*v >= '0' && *v <= '9') {
		v++;
	}
	if (*v != '.') {
		return 0;
	}
	v++;
	while (*v >= '0' && *v <= '9') {
		v++;
	}
	if (*v != '.') {
		return 0;
	}
	v++;
	if (*v < '0' || *v > '9') {
		return 0;
	}
	unsigned patch = 0U;

	while (*v >= '0' && *v <= '9') {
		patch = patch * 10U + (unsigned)(*v - '0');
		v++;
	}
	if (patch > 255U) {
		patch = 255U;
	}
	return (uint8_t)patch;
}

/* Steady advertising: fixed ~1000 ms (SoftDevice often sticks to interval_min
 * when given a window — the old 160–1000 ms range showed as continuous 160 ms).
 * Matches Firmware_NRF APP_ADV_INTERVAL. Boost still 20–30 ms for 3 s after
 * button/touch for faster rediscovery. */
#define OD_ADV_INTERVAL_MIN       BT_GAP_ADV_SLOW_INT_MIN /* 1600 = 1000 ms */
#define OD_ADV_INTERVAL_MAX       BT_GAP_ADV_SLOW_INT_MIN /* 1600 = 1000 ms */
#define OD_ADV_BOOST_INTERVAL_MIN 32u   /* 20 ms */
#define OD_ADV_BOOST_INTERVAL_MAX 48u   /* 30 ms */
#define OD_ADV_BOOST_MS           3000u

static struct GlobalConfig s_od_global_config;
static uint8_t msd_payload[MSD_PAYLOAD_LEN];
static uint8_t dynamic_return[11];
static char s_dev_name[16];
static struct bt_conn *s_conn;
static bool s_notify_enabled;
static bool s_adv_active;
static uint32_t s_adv_boost_until_ms;
static uint8_t s_msd_loop_counter;
static uint32_t s_last_adv_retry_ms;
static uint8_t s_reboot_flag = 1; /* set after boot, cleared on first BLE connect */
static uint8_t s_connection_requested; /* MSD status bit2; see opendisplay_ble_set_connection_requested */
static uint8_t s_last_published_msd[MSD_PAYLOAD_LEN];
static bool s_msd_published;
static bool s_adv_was_boosted;
static struct bt_le_adv_param s_adv_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN,
	OD_ADV_INTERVAL_MIN, OD_ADV_INTERVAL_MAX, NULL);

static struct k_work_delayable s_adv_restart_work;
static struct k_work_delayable s_dfu_work;
static struct k_work s_boot_display_work;
static struct k_work_q s_display_work_q;
static K_THREAD_STACK_DEFINE(s_display_wq_stack, 8192);
static bool s_display_wq_started;
static bool s_adv_work_msd_publish;
/* When encryption is on, SMP stays hidden until CMD_ENTER_DFU unlocks it
 * for this boot (Adafruit bledfu.begin() gating). */
static bool s_ota_unlocked;

static int start_advertising(void);
static bool publish_msd_to_advertising(void);
static void apply_tx_power(uint8_t handle_type, uint16_t handle);
static void od_smp_sync(void);
static void schedule_adv_restart(uint32_t delay_ms);
static void request_fast_link(struct bt_conn *conn);

/* Mirror Adafruit ble_nrf_request_fast_link(): ask for 2M + max DLE; central may decline. */
static void request_fast_link(struct bt_conn *conn)
{
	int err;

	if (conn == NULL) {
		return;
	}

#if defined(CONFIG_BT_CTLR_PHY_2M)
	err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (err != 0) {
		printf("[OD] PHY 2M request failed: %d\r\n", err);
	}
#endif

	err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (err != 0) {
		printf("[OD] DLE max request failed: %d\r\n", err);
	}

	err = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(6, 12, 0, 400));
	if (err != 0) {
		printf("[OD] conn param update failed: %d\r\n", err);
	}
}

static bool od_encryption_on(void)
{
	const struct SecurityConfig *sec = od_get_parsed_security();

	return (sec != NULL) && (sec->encryption_enabled != 0u);
}

static void od_smp_sync(void)
{
#if defined(CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION)
	/* smp_bt_setup() already registers the SVC at boot when dynamic reg is on. */
	static bool s_smp_visible = true;
	const bool want = s_ota_unlocked || !od_encryption_on();
	int err;

	if (want == s_smp_visible) {
		return;
	}
	if (want) {
		err = smp_bt_register();
		if (err != 0 && err != -EALREADY) {
			printf("[OD] SMP register failed: %d\r\n", err);
			return;
		}
		s_smp_visible = true;
		printf("[OD] SMP DFU service %s\r\n",
		       s_ota_unlocked ? "unlocked" : "available (encryption off)");
	} else {
		err = smp_bt_unregister();
		if (err != 0 && err != -ENOENT) {
			printf("[OD] SMP unregister failed: %d\r\n", err);
			return;
		}
		s_smp_visible = false;
		printf("[OD] SMP DFU service hidden (use CMD_ENTER_DFU)\r\n");
	}
#else
	ARG_UNUSED(s_ota_unlocked);
#endif
}

static void dfu_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	s_ota_unlocked = true;
	od_smp_sync();
	if (s_conn != NULL) {
		(void)bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		schedule_adv_restart(0);
	}
}

static void boot_display_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	opendisplay_display_boot_apply();
}

static void schedule_boot_display_apply(void)
{
	if (!s_display_wq_started) {
		k_work_queue_init(&s_display_work_q);
		k_work_queue_start(&s_display_work_q, s_display_wq_stack,
				   K_THREAD_STACK_SIZEOF(s_display_wq_stack), 14, NULL);
		s_display_wq_started = true;
	}
	(void)k_work_submit_to_queue(&s_display_work_q, &s_boot_display_work);
}

static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (s_conn != NULL) {
		s_adv_work_msd_publish = false;
		return;
	}
	if (s_adv_work_msd_publish) {
		s_adv_work_msd_publish = false;
		(void)publish_msd_to_advertising();
		return;
	}
	int err = start_advertising();
	if (err != 0) {
		printf("[OD] adv restart retry (err %d)\r\n", err);
		(void)k_work_schedule(&s_adv_restart_work, K_MSEC(200));
	}
}

static void schedule_adv_restart(uint32_t delay_ms)
{
	s_adv_work_msd_publish = false;
	(void)k_work_cancel_delayable(&s_adv_restart_work);
	(void)k_work_schedule(&s_adv_restart_work, K_MSEC(delay_ms));
}

static void schedule_msd_publish(void)
{
	s_adv_work_msd_publish = true;
	(void)k_work_cancel_delayable(&s_adv_restart_work);
	(void)k_work_schedule(&s_adv_restart_work, K_NO_WAIT);
}

static void chip_id_hex6(char out[7])
{
	uint8_t id[8];
	uint64_t uid = 0;

	(void)hwinfo_get_device_id(id, sizeof(id));
	for (unsigned i = 0; i < sizeof(id); i++) {
		uid = (uid << 8) | id[i];
	}
	snprintf(out, 7, "%06lX", (unsigned long)(uid & 0xFFFFFFu));
}

static float s_chip_temperature = -999.0f;

/*
 * With CONFIG_TEMP_NRF5_MPSL (SoftDevice builds) sample_fetch uses
 * mpsl_temperature_get() and is safe after bt_enable(). Legacy Zephyr temp
 * drivers block on DATARDY IRQ; keep this call after the stack is up.
 */
static void read_chip_temperature_once(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_temp)
	const struct device *temp_dev = DEVICE_DT_GET(DT_NODELABEL(temp));
	struct sensor_value val;

	if (device_is_ready(temp_dev) && sensor_sample_fetch(temp_dev) == 0 &&
	    sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &val) == 0) {
		s_chip_temperature = (float)sensor_value_to_double(&val);
	}
#endif
}

static void update_msd_payload(void)
{
	uint16_t battery_voltage_10mv;
	int16_t temp_encoded;
	uint8_t temperature_byte;
	uint8_t battery_voltage_low_byte;
	uint8_t status_byte;

	/* Mirror the reference updatemsdata() ordering: refresh the sensor
	 * dynamic slots, then the battery source (BQ27220-preferred, else SAADC),
	 * before packing the frame. All three are TTL-cached (30 s). */
	opendisplay_sensor_sht40_poll();
	opendisplay_sensor_bq27220_poll();
	opendisplay_sensor_npm1300_poll();
	battery_voltage_10mv = opendisplay_battery_get_10mv();

	temp_encoded = (int16_t)((s_chip_temperature + 40.0f) * 2.0f);
	if (temp_encoded < 0) {
		temp_encoded = 0;
	} else if (temp_encoded > 255) {
		temp_encoded = 255;
	}
	temperature_byte = (uint8_t)temp_encoded;
	battery_voltage_low_byte = (uint8_t)(battery_voltage_10mv & 0xFFu);
	/* Matches nRF52840 Firmware status byte (display_service.cpp:1293-1297):
	 * bit0 battery high bit, bit1 rebootFlag, bit2 connectionRequested,
	 * bits 4-7 loop counter. */
	status_byte = (uint8_t)(((battery_voltage_10mv >> 8) & 0x01u) |
				((s_reboot_flag & 0x01u) << 1) |
				((s_connection_requested & 0x01u) << 2) |
				((s_msd_loop_counter & 0x0Fu) << 4));

	memset(msd_payload, 0, sizeof(msd_payload));
	msd_payload[0] = (uint8_t)(OPENDISPLAY_COMPANY_ID & 0xFFu);
	msd_payload[1] = (uint8_t)((OPENDISPLAY_COMPANY_ID >> 8) & 0xFFu);
	memcpy(&msd_payload[2], dynamic_return, sizeof(dynamic_return));
	msd_payload[13] = temperature_byte;
	msd_payload[14] = battery_voltage_low_byte;
	msd_payload[15] = status_byte;
	s_msd_loop_counter = (uint8_t)((s_msd_loop_counter + 1u) & 0x0Fu);
}

static void log_msd(const char *tag)
{
#if defined(OD_LOW_POWER_QUIET)
	ARG_UNUSED(tag);
#else
	printf("[OD] msd %s:", tag);
	for (unsigned i = 0; i < MSD_PAYLOAD_LEN; i++) {
		printf(" %02X", msd_payload[i]);
	}
	printf("\r\n");
#endif
}

static ssize_t od_gatt_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	if (len == 0) {
		return 0;
	}
	opendisplay_pipe_on_write(buf, len, (flags & BT_GATT_WRITE_FLAG_CMD) != 0);
	return len;
}

static void od_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	s_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	opendisplay_pipe_on_notify_changed(s_notify_enabled);
}

BT_GATT_SERVICE_DEFINE(
	od_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0x2446)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x2446),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP |
				       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, od_gatt_write, NULL),
	BT_GATT_CCC(od_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, msd_payload, MSD_PAYLOAD_LEN),
};

static struct bt_data sd_name = BT_DATA(BT_DATA_NAME_COMPLETE, s_dev_name, 0);
static const struct bt_data sd_od_uuid = BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x46, 0x24);
static struct bt_data sd_cs_uuid;
static struct bt_data sd_buf[3];

static unsigned sd_prepare(void)
{
	unsigned count = 2u;
	unsigned cs_extra = 0u;

	sd_name.data_len = (uint8_t)strlen(s_dev_name);
	sd_buf[0] = sd_name;
	sd_buf[1] = sd_od_uuid;
	opendisplay_cs_fill_scan_response(&s_od_global_config, &sd_cs_uuid, 1u, &cs_extra);
	if (cs_extra != 0u) {
		sd_buf[2] = sd_cs_uuid;
		count = 3u;
	}
	return count;
}

static bool publish_msd_to_advertising(void)
{
	if (s_conn != NULL) {
		return false;
	}
	if (s_msd_published && memcmp(s_last_published_msd, msd_payload, MSD_PAYLOAD_LEN) == 0) {
		return false;
	}
	memcpy(s_last_published_msd, msd_payload, MSD_PAYLOAD_LEN);
	s_msd_published = true;
	log_msd("publish");

	if (s_adv_active) {
		unsigned sd_count = sd_prepare();

		if (bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd_buf, sd_count) == 0) {
			return true;
		}
		/* Fall through to restart only if update_data failed. */
	}
	return start_advertising() == 0;
}

void opendisplay_ble_update_msd(bool refresh_advertising)
{
	update_msd_payload();
	if (!refresh_advertising) {
		return;
	}
	/* Matches ESP32 Firmware: skip redundant adv refresh when payload unchanged. */
	if (s_msd_published && memcmp(s_last_published_msd, msd_payload, MSD_PAYLOAD_LEN) == 0) {
		return;
	}
	schedule_msd_publish();
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		printf("[OD] connect failed: %u\r\n", (unsigned)err);
		opendisplay_ble_boost_advertising();
		schedule_adv_restart(150);
		return;
	}
	(void)k_work_cancel_delayable(&s_adv_restart_work);
	s_conn = bt_conn_ref(conn);
	s_adv_active = false;
	s_reboot_flag = 0;
	request_fast_link(conn);
	if (opendisplay_cs_config_enabled(&s_od_global_config)) {
		opendisplay_cs_on_connected(conn);
	}
#if defined(CONFIG_BT_HCI_VS)
	{
		uint16_t conn_handle = 0;

		if (bt_hci_get_conn_handle(conn, &conn_handle) == 0) {
			apply_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_CONN, conn_handle);
		}
	}
#endif
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printf("[OD] disconnected reason=%u\r\n", (unsigned)reason);
	opendisplay_cs_on_disconnected(conn);
	opendisplay_pipe_on_connection_closed();
	if (s_conn != NULL) {
		bt_conn_unref(s_conn);
		s_conn = NULL;
	}
	s_adv_active = false;
	opendisplay_ble_boost_advertising();
	schedule_adv_restart(150);
}

static void recycled(void)
{
	/* Runs in ISR-like context: only queue work, no BT API calls here. */
	if (s_conn == NULL) {
		(void)k_work_schedule(&s_adv_restart_work, K_NO_WAIT);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

const struct GlobalConfig *opendisplay_get_global_config(void)
{
	return &s_od_global_config;
}

uint16_t opendisplay_ble_get_app_version(void)
{
	if (fw_build_version_string() != NULL) {
		return ((uint16_t)fw_major_from_build_version() << 8) |
		       fw_minor_from_build_version();
	}
	return OD_APP_VERSION;
}

uint8_t opendisplay_ble_get_app_version_patch(void)
{
	if (fw_build_version_string() != NULL) {
		return fw_patch_from_build_version();
	}
	return 0;
}

float opendisplay_ble_get_chip_temperature(void)
{
	return s_chip_temperature;
}

void opendisplay_ble_copy_msd_bytes(uint8_t out[16])
{
	log_msd("read 0x0044");
	memcpy(out, msd_payload, 16);
}

bool opendisplay_ble_pipe_notify(const uint8_t *data, uint16_t len)
{
	if (!s_notify_enabled || s_conn == NULL || len == 0) {
		return false;
	}
	return bt_gatt_notify(s_conn, &od_svc.attrs[2], data, len) == 0;
}

bool opendisplay_ble_pipe_notify_enabled(void)
{
	return s_notify_enabled;
}

void opendisplay_ble_pipe_on_write(const uint8_t *data, uint16_t len, bool write_cmd)
{
	opendisplay_pipe_on_write(data, len, write_cmd);
}

void opendisplay_ble_pipe_on_connection_closed(void)
{
	opendisplay_pipe_on_connection_closed();
}

void opendisplay_ble_set_connection_requested(bool requested)
{
	s_connection_requested = requested ? 1u : 0u;
}

void opendisplay_ble_set_dynamic_byte(uint8_t index, uint8_t value)
{
	if (index < sizeof(dynamic_return)) {
		dynamic_return[index] = value;
	}
}

/*
 * Apply the configured TX power (power_option.tx_power, dBm as a signed int8).
 * The reference nRF52840 build calls Bluefruit.setTxPower(power_option.tx_power)
 * once at init (ble_init.cpp:90). On Zephyr with the SoftDevice Controller there
 * is no stable bt_le_* runtime API for this, so use the standard HCI vendor-
 * specific Write_Tx_Power_Level command (as in Zephyr's hci_pwr_ctrl sample):
 * the controller clamps the requested value to its supported set and returns the
 * value it actually selected, which we log. handle_type selects advertising vs a
 * specific connection.
 */
static void apply_tx_power(uint8_t handle_type, uint16_t handle)
{
#if defined(CONFIG_BT_HCI_VS)
	int8_t requested = (int8_t)s_od_global_config.power_option.tx_power;
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (buf == NULL) {
		printf("[OD] tx_power: no HCI cmd buffer\r\n");
		return;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	cp->handle_type = handle_type;
	cp->tx_power_level = requested;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err != 0) {
		printf("[OD] tx_power set failed (type=%u req=%d dBm): %d\r\n",
		       (unsigned)handle_type, (int)requested, err);
		return;
	}
	rp = (struct bt_hci_rp_vs_write_tx_power_level *)rsp->data;
	printf("[OD] tx_power type=%u requested=%d selected=%d dBm\r\n",
	       (unsigned)handle_type, (int)requested, (int)rp->selected_tx_power);
	net_buf_unref(rsp);
#else
	ARG_UNUSED(handle_type);
	ARG_UNUSED(handle);
	printf("[OD] tx_power: CONFIG_BT_HCI_VS disabled; not applied\r\n");
#endif
}

static void apply_adv_interval(void)
{
	uint32_t now = k_uptime_get_32();

	if (s_adv_boost_until_ms != 0u && now < s_adv_boost_until_ms) {
		s_adv_param.interval_min = OD_ADV_BOOST_INTERVAL_MIN;
		s_adv_param.interval_max = OD_ADV_BOOST_INTERVAL_MAX;
	} else {
		s_adv_boost_until_ms = 0;
		s_adv_param.interval_min = OD_ADV_INTERVAL_MIN;
		s_adv_param.interval_max = OD_ADV_INTERVAL_MAX;
	}
}

static int start_advertising(void)
{
	char hex[7];
	int err;

	if (s_conn != NULL) {
		return 0;
	}

	chip_id_hex6(hex);
	snprintf(s_dev_name, sizeof(s_dev_name), "%s%s", OD_NAME_PREFIX, hex);
	unsigned sd_count = sd_prepare();
	/* MSD payload is updated only via opendisplay_ble_update_msd(), not on every
	 * adv stop/start (disconnect restart, boost end, retry fallback). */

	apply_adv_interval();
	(void)bt_le_adv_stop();
	err = bt_le_adv_start(&s_adv_param, ad, ARRAY_SIZE(ad), sd_buf, sd_count);
	s_adv_active = (err == 0);
	if (err != 0) {
		printf("[OD] adv start failed: %d (will retry)\r\n", err);
	} else {
		if (!s_msd_published) {
			memcpy(s_last_published_msd, msd_payload, MSD_PAYLOAD_LEN);
			s_msd_published = true;
		}
		printf("[OD] advertising as %s (interval=%u-%u ms)\r\n", s_dev_name,
		       (unsigned)BT_GAP_ADV_INTERVAL_TO_MS(s_adv_param.interval_min),
		       (unsigned)BT_GAP_ADV_INTERVAL_TO_MS(s_adv_param.interval_max));
	}
	return err;
}

/* Put a configured external SPI NOR flash into deep power-down after every
 * config load (matches nRF52840 Firmware powerDownExternalFlashFromConfig). */
static void flash_powerdown_from_config(void)
{
	if (!s_od_global_config.loaded) {
		return;
	}
	for (uint8_t i = 0; i < s_od_global_config.flash_config_count; i++) {
		const struct FlashConfig *fc = &s_od_global_config.flash_configs[i];

		if ((fc->flags & FLASH_CONFIG_FLAG_ENABLED) == 0u) {
			continue;
		}
		if (fc->mosi_pin == 0xFFu || fc->sck_pin == 0xFFu || fc->cs_pin == 0xFFu) {
			continue;
		}
		printf("[OD] flash powerdown MOSI=%u SCK=%u CS=%u MISO=%u WP=%u HOLD=%u\r\n",
		       fc->mosi_pin, fc->sck_pin, fc->cs_pin,
		       fc->miso_pin, fc->wp_pin, fc->hold_pin);
		board_nrf54_flash_powerdown(fc->mosi_pin, fc->sck_pin, fc->cs_pin,
					    fc->miso_pin, fc->wp_pin, fc->hold_pin);
		break;
	}
}

void opendisplay_ble_reload_config_from_nvm(void)
{
	if (!loadGlobalConfig(&s_od_global_config)) {
		memset(&s_od_global_config, 0, sizeof(s_od_global_config));
	}
	flash_powerdown_from_config();
	od_smp_sync();
	opendisplay_nfc_apply_config(&s_od_global_config);
	/* Re-apply advertising TX power in case the new config changed it. */
	apply_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0);
	if (s_conn == NULL) {
		schedule_adv_restart(0);
	}
}

void opendisplay_ble_restart_advertising(void)
{
	schedule_adv_restart(0);
}

void opendisplay_ble_boost_advertising(void)
{
	uint32_t now = k_uptime_get_32();
	const bool already_boosting =
		(s_adv_boost_until_ms != 0u && now < s_adv_boost_until_ms);

	s_adv_boost_until_ms = now + OD_ADV_BOOST_MS;
	/* Only restart advertising when entering boost (interval must change).
	 * Refreshing boost while already boosted must not stop/start ADV — NFC
	 * field chatter was spamming start_advertising(). */
	if (s_conn == NULL && !already_boosting) {
		schedule_adv_restart(0);
	}
}

void opendisplay_ble_advertising_tick(void)
{
	uint32_t now = k_uptime_get_32();
	const bool boosting = (s_adv_boost_until_ms != 0u && now < s_adv_boost_until_ms);

	if (boosting) {
		s_adv_was_boosted = true;
		return;
	}
	if (!s_adv_was_boosted || s_conn != NULL || !s_adv_active) {
		s_adv_was_boosted = false;
		s_adv_boost_until_ms = 0;
		return;
	}
	s_adv_was_boosted = false;
	s_adv_boost_until_ms = 0;
	schedule_adv_restart(0);
}

void opendisplay_ble_init(void)
{
	int err;

	(void)initConfigStorage();
#ifdef FACTORY_CLEAR_CONFIG_ON_BOOT
	/* One-shot clear build (scripts/factory_config_gen.py). */
	printf("[OD] factory clear build: erasing stored config\r\n");
	(void)clearStoredConfig();
#endif
	bool config_loaded = loadGlobalConfig(&s_od_global_config);
	if (!config_loaded && tryProvisionFactoryEmbed()) {
		/* No valid stored config, but a factory embed was just provisioned. */
		config_loaded = loadGlobalConfig(&s_od_global_config);
	}
	if (config_loaded) {
		printf("[OD] config loaded: displays=%u\r\n",
		       (unsigned)s_od_global_config.display_count);
	} else {
		printf("[OD] config: defaults\r\n");
	}
	flash_powerdown_from_config();

	opendisplay_sensor_bq27220_init();
	opendisplay_sensor_npm1300_init();
	opendisplay_sensor_sht40_init();

	opendisplay_led_init();
	opendisplay_buzzer_init();

	printf("[OD] enabling Bluetooth\r\n");
	err = bt_enable(NULL);
	if (err != 0) {
		printf("[OD] bt_enable failed: %d\r\n", err);
		return;
	}
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		(void)settings_load();
	}
	read_chip_temperature_once();

	opendisplay_button_init();
	opendisplay_touch_init();
	k_work_init_delayable(&s_adv_restart_work, adv_work_handler);
	k_work_init_delayable(&s_dfu_work, dfu_work_handler);
	k_work_init(&s_boot_display_work, boot_display_work_handler);
	/* After SoftDevice + adv work init (NFC field MSD uses schedule_msd_publish). */
	opendisplay_nfc_apply_config(&s_od_global_config);
	/* Match Adafruit: hide SMP when encryption is on until CMD_ENTER_DFU. */
	od_smp_sync();
	update_msd_payload();
	err = start_advertising();
	if (err != 0) {
		printf("[OD] initial adv failed: %d (will retry)\r\n", err);
		schedule_adv_restart(0);
	} else {
		apply_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0);
	}
	printf("[OD] BLE ready as %s\r\n", s_dev_name);
	schedule_boot_display_apply();
}

void opendisplay_ble_process(void)
{
	uint32_t now = k_uptime_get_32();

	opendisplay_pipe_process();
	opendisplay_led_process();
	opendisplay_buzzer_process();
	opendisplay_button_process();
	opendisplay_touch_process();
	opendisplay_nfc_process();
	opendisplay_ble_advertising_tick();

	/* Fallback if the delayed work restart fails or was cancelled. */
	if (s_conn == NULL && !s_adv_active && (now - s_last_adv_retry_ms) >= 500u) {
		s_last_adv_retry_ms = now;
		schedule_adv_restart(0);
	}
}

void opendisplay_ble_schedule_dfu(void)
{
	printf("[OD] ENTER_DFU: unlocking SMP OTA\r\n");
	(void)k_work_cancel_delayable(&s_dfu_work);
	(void)k_work_schedule(&s_dfu_work, K_MSEC(500));
}

void opendisplay_ble_schedule_deep_sleep(void)
{
	printf("[OD] deep sleep: nPM1300 hibernate if available\r\n");
	opendisplay_sensor_npm1300_enter_hibernate();
}

bool opendisplay_ble_is_connected(void)
{
	return s_conn != NULL;
}
