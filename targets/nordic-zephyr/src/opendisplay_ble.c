#include "opendisplay_ble.h"
#include "od_log.h"
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
#include "od_board.h"
#include "od_advert.h"

#include <stdio.h>
#include <string.h>

#include "od_rxq.h"
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

#define MSD_PAYLOAD_LEN        OD_ADVERT_MSD_LEN
#define OD_NAME_PREFIX         "OD"
/* Set by zephyr/CMakeLists.txt, which explains why this is not a Kconfig. Mirrored here so the
 * file still compiles if it is ever built outside that CMake. */
#ifndef OD_TX_POWER_DBM
#define OD_TX_POWER_DBM 8
#endif
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

static struct od_config s_od_global_config;
static uint8_t msd_payload[MSD_PAYLOAD_LEN];
static uint8_t dynamic_return[OD_ADVERT_DYNAMIC_LEN];
static char s_dev_name[16];
static struct bt_conn *s_conn;
static bool s_notify_enabled;
static bool s_adv_active;
static uint32_t s_adv_boost_start_ms;
static bool s_adv_boost_on;
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
/* Gives once the boot-display work item returns, success or failure. opendisplay_ble_init()
 * blocks on this before start_advertising() -- see its call site for why. */
static struct k_sem s_boot_display_done;
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
		od_log_info("PHY 2M request failed: %d", err);
	}
#endif

	err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (err != 0) {
		od_log_info("DLE max request failed: %d", err);
	}

	err = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(6, 12, 0, 400));
	if (err != 0) {
		od_log_info("conn param update failed: %d", err);
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
			od_log_info("SMP register failed: %d", err);
			return;
		}
		s_smp_visible = true;
		od_log_info("SMP DFU service %s",
		       s_ota_unlocked ? "unlocked" : "available (encryption off)");
	} else {
		err = smp_bt_unregister();
		if (err != 0 && err != -ENOENT) {
			od_log_info("SMP unregister failed: %d", err);
			return;
		}
		s_smp_visible = false;
		od_log_info("SMP DFU service hidden (use CMD_ENTER_DFU)");
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
	if (!opendisplay_display_boot_apply()) {
		od_log_error("boot display failed after bounded retry");
	}
	/* Given on both the success and failure arms above: the waiter in opendisplay_ble_init()
	 * cares that the panel is no longer being touched, not that the render succeeded. */
	k_sem_give(&s_boot_display_done);
}

/* Deliberately shorter than opendisplay_display_boot_apply()'s worst legitimate case (two
 * attempts of ~900 ms rail settle + a 60 s wait_for_refresh() each, ~125 s): BLE reachability
 * within 30 s wins over waiting out a slow-but-healthy render. A render still in flight past
 * this point keeps running on s_display_work_q after advertising opens -- the race the wait
 * exists to close (see the call site) is narrowed, not eliminated, for that tail case. */
#define OD_BOOT_DISPLAY_WAIT_MS 30000u

static void schedule_boot_display_apply(void)
{
	if (!s_display_wq_started) {
		k_sem_init(&s_boot_display_done, 0, 1);
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
		od_log_info("adv restart retry (err %d)", err);
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

uint32_t opendisplay_ble_chip_id_last24(void)
{
	uint8_t id[8];
	uint64_t uid = 0;

	(void)hwinfo_get_device_id(id, sizeof(id));
	for (unsigned i = 0; i < sizeof(id); i++) {
		uid = (uid << 8) | id[i];
	}
	/*
	 * THIS IS THE DISPLAY/ADVERTISING IDENTITY ONLY. The session-auth device_id is a
	 * DIFFERENT FICR word by contract -- getAuthDeviceIdBytes() uses DEVICEID[0] while
	 * getChipIdHex() uses DEVICEID[1] -- so opendisplay_pipe.c derives its own and must
	 * not be routed through here. Aligning them would change every derived session key.
	 *
	 * WHICH FICR WORD THIS TAKES IS A COMPATIBILITY CONTRACT, NOT A DETAIL.
	 *
	 * Zephyr's nRF hwinfo driver returns be32(DEVICEID[1]) || be32(DEVICEID[0])
	 * (zephyr/drivers/hwinfo/hwinfo_nrf.c) -- the two words are SWAPPED relative to the
	 * register order. Accumulating big-endian therefore yields
	 *     uid = (DEVICEID[1] << 32) | DEVICEID[0]
	 * so a plain `uid & 0xFFFFFF` is the low 3 bytes of DEVICEID[0].
	 *
	 * The Arduino nRF52 firmware names the device from DEVICEID[**1**] & 0xFFFFFF
	 * (Firmware/src/encryption.cpp getChipIdHex). So on the nRF52840 the migrated
	 * firmware advertised a DIFFERENT OD<id> than the same physical board did under the
	 * old firmware -- reported from hardware as "OD address is incorrect/different".
	 *
	 * Fixed only for the nRF52840, deliberately. The nRF54 boards have never had an
	 * Arduino firmware to agree with, and their current names are already deployed;
	 * "correcting" them here would rename every field unit for no benefit.
	 */
#if defined(OD_BOARD_XIAO_NRF52840)
	uid >>= 32; /* DEVICEID[1] -- match the Arduino nRF52 firmware. */
#endif
	return (uint32_t)(uid & 0xFFFFFFu);
}

/* Every consumer of the advertised identity goes through opendisplay_ble_chip_id_last24():
 * the boot screen used to re-derive it and omitted the board conditional above, so an
 * nRF52840 printed one OD<id> on screen and advertised another. */
static void chip_id_hex6(char out[7])
{
	snprintf(out, 7, "%06lX", (unsigned long)opendisplay_ble_chip_id_last24());
}

static float s_chip_temperature = -999.0f;

/*
 * With CONFIG_TEMP_NRF5_MPSL (SoftDevice builds) sample_fetch uses
 * mpsl_temperature_get() and is safe after bt_enable(). Legacy Zephyr temp
 * drivers block on DATARDY IRQ; keep this call after the stack is up.
 */
static void read_chip_temperature(void)
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
	struct od_advert_inputs adv;
	uint16_t battery_voltage_10mv;

	/* Mirror the reference updatemsdata() ordering: refresh the sensor
	 * dynamic slots, then the battery source (BQ27220-preferred, else SAADC),
	 * before packing the frame. All three are TTL-cached (30 s). */
	opendisplay_sensor_sht40_poll();
	opendisplay_sensor_bq27220_poll();
	opendisplay_sensor_npm1300_poll();
	battery_voltage_10mv = opendisplay_battery_get_10mv();
	/* Reference updatemsdata() reads the die temperature per publish; every caller
	 * of this function runs after bt_enable(). */
	read_chip_temperature();

	/* Encoding -- company id, the (t + 40) * 2 temperature step, the 10-bit battery
	 * split across battery_voltage_low and status bit0, the status bit positions --
	 * belongs to shared/core/od_advert.c. What stays here is acquisition: the polls
	 * above, and a battery module that already reports the wire's 10 mV units. */
	memset(&adv, 0, sizeof(adv));
	adv.dynamic = dynamic_return;
	adv.chip_temperature_c = s_chip_temperature;
	adv.battery_10mv = battery_voltage_10mv;
	adv.reboot_flag = (s_reboot_flag != 0u);
	adv.connection_requested = (s_connection_requested != 0u);
	adv.loop_counter = s_msd_loop_counter;
	od_advert_build(&adv, msd_payload);

	s_msd_loop_counter = od_advert_advance_counter(s_msd_loop_counter);
}

static void log_msd(int level, const char *tag)
{
#if defined(OD_LOW_POWER_QUIET)
	ARG_UNUSED(level);
	ARG_UNUSED(tag);
#else
	/* Gate BEFORE formatting, using the constant-fold `if` idiom od_log.h documents rather
	 * than `#if`: the body still compiles at every level, but a build below the caller's
	 * level does no snprintf work. The od_log_*() macros gate inside themselves, so they
	 * cannot elide the argument formatting this helper does -- and this runs on every MSD
	 * publish and every 0x0044 read. */
	if (OD_LOG_LEVEL >= level) {
		char label[32];
		char line[96];

		snprintf(label, sizeof(label), "[OD] msd %s: ", tag);
		od_log_hex_line(line, sizeof(line), label, msd_payload, MSD_PAYLOAD_LEN);
		_od_log(level, "%s", line);
	}
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
	/* ADMISSION AT THE ATT LAYER, matching ESP32. Over-length writes are refused with ATT 0x0D
	 * (Invalid Attribute Value Length) rather than accepted and dropped at the queue: a host can
	 * discover the first and cannot discover the second. This also keeps the 245..256 band
	 * reachable, where the DISPATCHER answers {0xFF,cmd,0xFE} -- so a client learns whether its
	 * frame was too big for the transport or too big for the protocol. */
	if (len > OD_RXQ_FRAME_MAX) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
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
	log_msd(OD_LOG_INFO, "publish");

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
		od_log_info("connect failed: %u", (unsigned)err);
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
	od_log_info("disconnected reason=%u", (unsigned)reason);
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

const struct od_config *opendisplay_get_global_config(void)
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
	/* DEBUG, not INFO: a polling host emits one of these per read. Matches the ESP32,
	 * which logs the read at debug and the publish at info. */
	log_msd(OD_LOG_DEBUG, "read 0x0044");
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
	/* Config wins when it states a power; 0 means "unstated" and takes the build default.
	 *
	 * The wire field is one UNSIGNED byte read as signed dBm, so every level the radio supports
	 * stays expressible except exactly 0 dBm -- -8 dBm is 0xF8, not 0. What 0 actually is, in
	 * practice, is the zero-filled value carried by a device whose config omits the field or that
	 * has no stored config at all, and 0 dBm is never what a battery tag on a weak link wants.
	 * Treating it as "unstated" is therefore a reinterpretation of exactly one value, and the
	 * only one where the wire cannot distinguish a request from a default. */
	int8_t configured = (int8_t)s_od_global_config.power_option.tx_power;
	int8_t requested = (configured != 0) ? configured : (int8_t)OD_TX_POWER_DBM;
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (buf == NULL) {
		od_log_info("tx_power: no HCI cmd buffer");
		return;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	cp->handle_type = handle_type;
	cp->tx_power_level = requested;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err != 0) {
		od_log_info("tx_power set failed (type=%u req=%d dBm): %d",
		       (unsigned)handle_type, (int)requested, err);
		return;
	}
	rp = (struct bt_hci_rp_vs_write_tx_power_level *)rsp->data;
	od_log_info("tx_power type=%u requested=%d selected=%d dBm",
	       (unsigned)handle_type, (int)requested, (int)rp->selected_tx_power);
	net_buf_unref(rsp);
#else
	ARG_UNUSED(handle_type);
	ARG_UNUSED(handle);
	od_log_info("tx_power: CONFIG_BT_HCI_VS disabled; not applied");
#endif
}

/*
 * ELAPSED SINCE START, never a stored deadline. k_uptime_get_32() wraps every 49.7 days, and
 * an absolute `now < deadline` test reads false for a boost that straddles the wrap, ending
 * it the moment it begins. Unsigned subtraction is correct across the wrap. The separate flag
 * carries "boosting at all" so no uptime value has to double as a not-boosting sentinel.
 */
static bool adv_boost_active(uint32_t now)
{
	return s_adv_boost_on && ((now - s_adv_boost_start_ms) < OD_ADV_BOOST_MS);
}

static void apply_adv_interval(void)
{
	if (adv_boost_active(k_uptime_get_32())) {
		s_adv_param.interval_min = OD_ADV_BOOST_INTERVAL_MIN;
		s_adv_param.interval_max = OD_ADV_BOOST_INTERVAL_MAX;
	} else {
		s_adv_boost_on = false;
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
		od_log_info("adv start failed: %d (will retry)", err);
	} else {
		if (!s_msd_published) {
			memcpy(s_last_published_msd, msd_payload, MSD_PAYLOAD_LEN);
			s_msd_published = true;
		}
		od_log_info("advertising as %s (interval=%u-%u ms)", s_dev_name,
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

		if ((fc->flags & OD_FLASH_FLAG_ENABLED) == 0u) {
			continue;
		}
		if (fc->mosi_pin == 0xFFu || fc->sck_pin == 0xFFu || fc->cs_pin == 0xFFu) {
			continue;
		}
		/* MISO / WP / HOLD are reserved[0..2] in the canonical contract, NOT named fields.
		 * The subset header this target used to carry named them miso_pin / wp_pin /
		 * hold_pin; canonical has never carved them out of FlashConfig.reserved, so the
		 * host does not know to set them and they arrive as the must-be-zero the contract
		 * promises unless a config was hand-built against the old nRF header. Reading them
		 * by index keeps the behaviour byte-identical while stating what they actually are.
		 * Promoting them is an upstream change (see protocol_pending.h for the sequence),
		 * not a local rename. */
		od_log_info("flash powerdown MOSI=%u SCK=%u CS=%u MISO=%u WP=%u HOLD=%u",
		       fc->mosi_pin, fc->sck_pin, fc->cs_pin,
		       fc->reserved[0], fc->reserved[1], fc->reserved[2]);
		od_board_flash_powerdown(fc->mosi_pin, fc->sck_pin, fc->cs_pin,
					 fc->reserved[0], fc->reserved[1], fc->reserved[2]);
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
	const bool already_boosting = adv_boost_active(now);

	s_adv_boost_start_ms = now;
	s_adv_boost_on = true;
	/* Only restart advertising when entering boost (interval must change).
	 * Refreshing boost while already boosted must not stop/start ADV — NFC
	 * field chatter was spamming start_advertising(). */
	if (s_conn == NULL && !already_boosting) {
		schedule_adv_restart(0);
	}
}

void opendisplay_ble_advertising_tick(void)
{
	if (adv_boost_active(k_uptime_get_32())) {
		s_adv_was_boosted = true;
		return;
	}
	if (!s_adv_was_boosted || s_conn != NULL || !s_adv_active) {
		s_adv_was_boosted = false;
		s_adv_boost_on = false;
		return;
	}
	s_adv_was_boosted = false;
	s_adv_boost_on = false;
	schedule_adv_restart(0);
}

void opendisplay_ble_init(void)
{
	int err;

	(void)initConfigStorage();
#ifdef FACTORY_CLEAR_CONFIG_ON_BOOT
	/* One-shot clear build (scripts/factory_config_gen.py). */
	od_log_info("factory clear build: erasing stored config");
	(void)clearStoredConfig();
#endif
	bool config_loaded = loadGlobalConfig(&s_od_global_config);
	if (!config_loaded && tryProvisionFactoryEmbed()) {
		/* No valid stored config, but a factory embed was just provisioned. */
		config_loaded = loadGlobalConfig(&s_od_global_config);
	}
	if (config_loaded) {
		od_log_info("config loaded: displays=%u",
		       (unsigned)s_od_global_config.display_count);
	} else {
		od_log_info("config: defaults");
	}
	flash_powerdown_from_config();

	opendisplay_sensor_bq27220_init();
	opendisplay_sensor_npm1300_init();
	opendisplay_sensor_sht40_init();

	opendisplay_led_init();
	opendisplay_buzzer_init();

	/* Before bt_enable(): its failure path returns early, and the main loop keeps
	 * calling opendisplay_ble_process(), whose adv fallback schedules this work.
	 * A work item scheduled before k_work_init() runs a NULL handler. */
	k_work_init_delayable(&s_adv_restart_work, adv_work_handler);
	k_work_init_delayable(&s_dfu_work, dfu_work_handler);
	k_work_init(&s_boot_display_work, boot_display_work_handler);

	od_log_info("enabling Bluetooth");
	err = bt_enable(NULL);
	if (err != 0) {
		od_log_info("bt_enable failed: %d", err);
		return;
	}
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		(void)settings_load();
	}

	opendisplay_button_init();
	opendisplay_touch_init();
	/* After SoftDevice + adv work init (NFC field MSD uses schedule_msd_publish). */
	opendisplay_nfc_apply_config(&s_od_global_config);
	/* Match Adafruit: hide SMP when encryption is on until CMD_ENTER_DFU. */
	od_smp_sync();
	update_msd_payload();

	/* BLOCKS until the boot screen is off the panel, or the bound below expires. Matches
	 * esp32-idf/main.cpp's "SoftDevice must start before display/SPI; advertising starts
	 * after boot screen" -- the BT stack is already up (bt_enable() above), but nothing may
	 * be able to CONNECT yet, because a connected central can push a direct-write immediately,
	 * and opendisplay_display_direct_write_start() touches the same BBEPDISP s_epd and the
	 * same panel GPIOs/SPI device the boot-display work queue thread is mid-sequence on, with
	 * no lock between them. A wedge on either side used to surface as a frozen, unpainted
	 * boot screen with no way to tell why. The timeout keeps a genuinely stuck render from
	 * also taking BLE reachability down with it -- staying reachable over BLE/DFU even with a
	 * bad panel is the same trade-off od_watchdog_app_safe_mode() makes elsewhere. */
	schedule_boot_display_apply();
	if (k_sem_take(&s_boot_display_done, K_MSEC(OD_BOOT_DISPLAY_WAIT_MS)) != 0) {
		od_log_error("boot display: still running after %u ms - advertising anyway",
			     (unsigned)OD_BOOT_DISPLAY_WAIT_MS);
	}

	err = start_advertising();
	if (err != 0) {
		od_log_info("initial adv failed: %d (will retry)", err);
		schedule_adv_restart(0);
	} else {
		apply_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0);
	}
	od_log_info("BLE ready as %s", s_dev_name);
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
	od_log_info("ENTER_DFU: unlocking SMP OTA");
	(void)k_work_cancel_delayable(&s_dfu_work);
	(void)k_work_schedule(&s_dfu_work, K_MSEC(500));
}

void opendisplay_ble_schedule_deep_sleep(void)
{
	od_log_info("deep sleep: nPM1300 hibernate if available");
	opendisplay_sensor_npm1300_enter_hibernate();
}

bool opendisplay_ble_is_connected(void)
{
	return s_conn != NULL;
}
