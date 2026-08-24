#include "opendisplay_sensor_npm1300.h"
#include "od_log.h"
#include "opendisplay_sensor_common.h"
#include "od_hal_i2c.h"
/* OD_SENSOR_TYPE_NPM1300 (6) is not in the canonical contract yet -- see the header. */
#include "protocol_pending.h"
#include "opendisplay_ble.h"
#include "od_runtime_types.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#define NPM1300_DEFAULT_ADDR_7BIT 0x6Bu
#define NPM1300_MSD_CHARGING_BIT  0x80u
#define NPM1300_MSD_POLL_TTL_MS   30000u

#define NPM1300_ADC_BASE   0x05u
#define NPM1300_CHGR_BASE  0x03u
#define NPM1300_SHIP_BASE  0x0Bu
#define NPM1300_TIME_BASE  0x07u

#define NPM1300_ADC_TASK_VBAT  0x00u
#define NPM1300_ADC_RESULTS    0x10u
#define NPM1300_CHGR_CHG_STAT  0x34u
#define NPM1300_SHIP_HIBERNATE 0x00u
#define NPM1300_TIME_TIMER     0x08u
#define NPM1300_TIME_LOAD      0x03u

#define NPM1300_ADC_CONV_TIME_US 250u
#define NPM1300_ADC_MSB_SHIFT    2u
#define NPM1300_ADC_LSB_MASK     0x03u

static float s_batt_v = -1.0f;
static bool s_gauge_ok;
static bool s_charging;

static const struct SensorData *npm1300_config(void)
{
	const struct od_config *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return NULL;
	}
	for (uint8_t i = 0; i < cfg->sensor_count; i++) {
		if (cfg->sensors[i].sensor_type == OD_SENSOR_TYPE_NPM1300) {
			return &cfg->sensors[i];
		}
	}
	return NULL;
}

static uint8_t npm1300_addr_7bit(const struct SensorData *s)
{
	uint8_t a = s->i2c_addr_7bit;

	if (a == 0u || a == 0xFFu) {
		return NPM1300_DEFAULT_ADDR_7BIT;
	}
	return a;
}

static bool npm1300_reg_write(const struct SensorData *s, uint8_t base, uint8_t offset,
			      const uint8_t *data, size_t len)
{
	struct od_i2c_bus bus;
	uint8_t buf[8];

	if (len + 2u > sizeof(buf) || s->bus_id == 0xFFu) {
		return false;
	}
	buf[0] = base;
	buf[1] = offset;
	if (len > 0u) {
		memcpy(&buf[2], data, len);
	}
	return od_hal_i2c_write(s->bus_id, npm1300_addr_7bit(s), buf,
				(uint16_t)(2u + len)) == OD_HAL_I2C_OK;
}

static bool npm1300_reg_write_u8(const struct SensorData *s, uint8_t base, uint8_t offset,
				 uint8_t value)
{
	return npm1300_reg_write(s, base, offset, &value, 1u);
}

static bool npm1300_reg_read(const struct SensorData *s, uint8_t base, uint8_t offset,
			     uint8_t *data, size_t len)
{
	struct od_i2c_bus bus;
	uint8_t hdr[2] = {base, offset};

	if (s->bus_id == 0xFFu) {
		return false;
	}
	/* Repeated START: the register pointer and the read are one transaction. */
	return od_hal_i2c_write_read(s->bus_id, npm1300_addr_7bit(s), hdr, sizeof(hdr),
				     data, (uint16_t)len) == OD_HAL_I2C_OK;
}

static uint8_t soc_from_voltage(float volts)
{
	if (volts < 3.30f) {
		return 0u;
	}
	if (volts >= 4.20f) {
		return 100u;
	}
	return (uint8_t)((volts - 3.30f) * (100.0f / 0.90f));
}

static bool npm1300_sample(const struct SensorData *s)
{
	/*
	 * Trigger VBAT (+ TEMP + DIE tasks as consecutive writes, matching the
	 * Zephyr npm13xx_charger sample_fetch burst), wait, then read results.
	 * Voltage: millivolts = code * 5000 / 1024 (datasheet / Zephyr driver).
	 */
	const uint8_t tasks[3] = {1u, 1u, 1u};
	uint8_t results[11];
	uint8_t chg_stat = 0u;

	if (s->bus_id == 0xFFu) {
		od_log_info("nPM1300: no data_bus assigned (bus_id 0xFF); not probed");
		return false;
	}

	if (!npm1300_reg_write(s, NPM1300_ADC_BASE, NPM1300_ADC_TASK_VBAT, tasks, sizeof(tasks))) {
		od_log_info("nPM1300: ADC task NACK (addr=0x%02X bus=%u)",
		       (unsigned)npm1300_addr_7bit(s), (unsigned)s->bus_id);
		return false;
	}

	(void)npm1300_reg_read(s, NPM1300_CHGR_BASE, NPM1300_CHGR_CHG_STAT, &chg_stat, 1u);
	k_busy_wait(NPM1300_ADC_CONV_TIME_US * 4u);

	if (!npm1300_reg_read(s, NPM1300_ADC_BASE, NPM1300_ADC_RESULTS, results, sizeof(results))) {
		od_log_info("nPM1300: ADC results read failed");
		return false;
	}

	uint8_t msb_vbat = results[1];
	uint8_t lsb_a = results[5];
	uint16_t code = (uint16_t)(((uint16_t)msb_vbat << NPM1300_ADC_MSB_SHIFT) |
				   (lsb_a & NPM1300_ADC_LSB_MASK));
	int32_t mv = (int32_t)code * 5000 / 1024;

	s_batt_v = (float)mv / 1000.0f;
	s_gauge_ok = s_batt_v > 0.5f;
	s_charging = ((chg_stat & 0x0Fu) == 0x0Cu || (chg_stat & 0x0Fu) == 0x0Du ||
		      (chg_stat & 0x0Fu) == 0x0Fu);
	if (!s_gauge_ok) {
		od_log_info("nPM1300: VBAT code=%u -> %d mV", (unsigned)code, (int)mv);
	}
	return s_gauge_ok;
}

static bool npm1300_sample_retries(const struct SensorData *s, unsigned attempts)
{
	for (unsigned i = 0u; i < attempts; i++) {
		if (npm1300_sample(s)) {
			return true;
		}
		k_msleep(5);
	}
	return false;
}

bool opendisplay_sensor_npm1300_is_available(void)
{
	const struct SensorData *s = npm1300_config();

	/* "Available" means a bus was assigned and it resolves to a usable I2C record. A probe
	 * would be a transaction; this is the same question the old bus construction answered. */
	return s != NULL && s->bus_id != 0xFFu &&
	       od_hal_i2c_probe(s->bus_id, npm1300_addr_7bit(s)) != OD_HAL_I2C_EINVAL;
}

bool opendisplay_sensor_npm1300_is_configured(void)
{
	return npm1300_config() != NULL;
}

float opendisplay_sensor_npm1300_voltage_volts(void)
{
	return s_gauge_ok ? s_batt_v : -1.0f;
}

static void npm1300_publish_msd(const struct SensorData *s)
{
	uint8_t msd_idx;
	uint8_t soc;
	uint8_t packed;

	if (s == NULL || s->msd_data_start_byte == 0xFFu || s->msd_data_start_byte > 10u) {
		return;
	}
	msd_idx = s->msd_data_start_byte;
	soc = s_gauge_ok ? soc_from_voltage(s_batt_v) : 0xFFu;
	if (!s_gauge_ok || soc > 100u) {
		opendisplay_ble_set_dynamic_byte(msd_idx, 0xFFu);
		return;
	}
	packed = (uint8_t)(soc & 0x7Fu);
	if (s_charging) {
		packed |= NPM1300_MSD_CHARGING_BIT;
	}
	opendisplay_ble_set_dynamic_byte(msd_idx, packed);
}

void opendisplay_sensor_npm1300_init(void)
{
	const struct SensorData *s = npm1300_config();

	if (s == NULL) {
		return;
	}
	k_msleep(30);
	if (!npm1300_sample_retries(s, 5u)) {
		od_log_info("nPM1300: I2C sample failed (bus/config)");
		return;
	}
	od_log_info("nPM1300: VBAT=%d mV (config I2C)", (int)(s_batt_v * 1000.0f));
	npm1300_publish_msd(s);
}

void opendisplay_sensor_npm1300_poll(void)
{
	static uint32_t last_poll_ms;
	static bool have_polled;

	const struct SensorData *s = npm1300_config();

	if (s == NULL) {
		return;
	}
	if (s->msd_data_start_byte == 0xFFu) {
		return;
	}

	uint32_t now = k_uptime_get_32();

	if (have_polled && (now - last_poll_ms) < NPM1300_MSD_POLL_TTL_MS) {
		return;
	}
	last_poll_ms = now;
	have_polled = true;

	if (!npm1300_sample_retries(s, 3u)) {
		od_log_info("nPM1300: sample failed, keeping last reading");
		npm1300_publish_msd(s);
		return;
	}

	npm1300_publish_msd(s);
}

void opendisplay_sensor_npm1300_enter_hibernate(void)
{
	const struct SensorData *s = npm1300_config();
	uint8_t timer[3] = {0u, 0u, 0u};

	if (s == NULL) {
		od_log_info("nPM1300: hibernate skipped (no sensor config)");
		return;
	}
	od_log_info("nPM1300: entering hibernate");
	(void)npm1300_reg_write(s, NPM1300_TIME_BASE, NPM1300_TIME_TIMER, timer, sizeof(timer));
	(void)npm1300_reg_write_u8(s, NPM1300_TIME_BASE, NPM1300_TIME_LOAD, 1u);
	k_msleep(1);
	(void)npm1300_reg_write_u8(s, NPM1300_SHIP_BASE, NPM1300_SHIP_HIBERNATE, 1u);
}
