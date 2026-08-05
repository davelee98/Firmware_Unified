#include "opendisplay_sensor_bq27220.h"
#include "opendisplay_sensor_common.h"
#include "opendisplay_ble.h"
#include "opendisplay_structs.h"
#include "nrf54_gpio.h"

#include <stdio.h>
#include <zephyr/kernel.h>

#define BQ27220_CMD_VOLTAGE      0x08u
#define BQ27220_CMD_SOC          0x2Cu
#define BQ27220_MSD_CHARGING_BIT 0x80u
#define BQ27220_MSD_POLL_TTL_MS  30000u

static float s_batt_v = -1.0f;
static bool s_gauge_ok;

static bool valid_pin(uint8_t pin)
{
	return pin != 0u && pin != 0xFFu;
}

static const struct SensorData *bq27220_config(void)
{
	const struct GlobalConfig *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return NULL;
	}
	for (uint8_t i = 0; i < cfg->sensor_count; i++) {
		if (cfg->sensors[i].sensor_type == SENSOR_TYPE_BQ27220) {
			return &cfg->sensors[i];
		}
	}
	return NULL;
}

static uint8_t bq27220_addr_7bit(const struct SensorData *s)
{
	uint8_t a = s->i2c_addr_7bit;

	if (a == 0u || a == 0xFFu) {
		return 0x55u;
	}
	return a;
}

/* Register read: write command byte (repeated start, no STOP) then read len. */
static bool bq27220_read_block(const struct SensorData *s, uint8_t cmd,
			       uint8_t *buf, uint8_t len)
{
	struct od_i2c_bus bus;

	if (!od_sensor_bus_for(s->bus_id, &bus)) {
		return false;
	}
	uint8_t addr = bq27220_addr_7bit(s);

	if (!od_i2c_write(&bus, addr, &cmd, 1, false)) {
		return false;
	}
	return od_i2c_read(&bus, addr, buf, len);
}

static bool charger_gpio_charging(void)
{
	const struct GlobalConfig *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return false;
	}
	uint8_t st = cfg->power_option.charge_state_pin;

	if (!valid_pin(st)) {
		return false;
	}
	bool active_low = (cfg->power_option.charger_flags & CHARGER_FLAG_STATE_ACTIVE_LOW) != 0u;
	int level = nrf54_gpio_read(st);

	/* Matches reference charger_gpio_charging(). */
	return active_low ? (level == 1) : (level == 0);
}

bool opendisplay_sensor_bq27220_is_configured(void)
{
	return bq27220_config() != NULL;
}

float opendisplay_sensor_bq27220_voltage_volts(void)
{
	return s_gauge_ok ? s_batt_v : -1.0f;
}

void opendisplay_sensor_bq27220_init(void)
{
	const struct GlobalConfig *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return;
	}
	uint8_t en = cfg->power_option.charge_enable_pin;

	if (valid_pin(en)) {
		bool active_low = (cfg->power_option.charger_flags &
				   CHARGER_FLAG_ENABLE_ACTIVE_LOW) != 0u;
		nrf54_gpio_configure_output(en, !active_low);
	}
	uint8_t st = cfg->power_option.charge_state_pin;

	if (valid_pin(st)) {
		nrf54_gpio_configure_input(st, true, false);
	}

	const struct SensorData *s = bq27220_config();

	if (s == NULL) {
		return;
	}
	uint8_t raw[2];

	if (!bq27220_read_block(s, BQ27220_CMD_VOLTAGE, raw, 2)) {
		printf("[OD] BQ27220: not found @0x%02X\r\n", bq27220_addr_7bit(s));
		return;
	}
	printf("[OD] BQ27220: fuel gauge @0x%02X\r\n", bq27220_addr_7bit(s));
}

void opendisplay_sensor_bq27220_poll(void)
{
	static uint32_t last_poll_ms;
	static bool have_polled;

	const struct SensorData *s = bq27220_config();

	if (s == NULL) {
		return;
	}
	uint32_t now = k_uptime_get_32();

	if (have_polled && (now - last_poll_ms) < BQ27220_MSD_POLL_TTL_MS) {
		return;
	}
	last_poll_ms = now;
	have_polled = true;

	uint8_t raw[2];

	if (!bq27220_read_block(s, BQ27220_CMD_VOLTAGE, raw, 2)) {
		s_gauge_ok = false;
		s_batt_v = -1.0f;
		return;
	}
	uint16_t mv = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);

	s_batt_v = mv / 1000.0f;
	s_gauge_ok = mv > 0u;

	uint8_t soc = 0xFFu;

	if (bq27220_read_block(s, BQ27220_CMD_SOC, &soc, 1)) {
		if (soc > 100u) {
			soc = 100u;
		}
	} else {
		soc = 0xFFu;
	}

	bool charging = charger_gpio_charging();
	uint8_t msd_idx = s->msd_data_start_byte;

	if (msd_idx <= 10u) {
		if (!s_gauge_ok || soc > 100u) {
			opendisplay_ble_set_dynamic_byte(msd_idx, 0xFFu);
		} else {
			uint8_t packed = (uint8_t)(soc & 0x7Fu);

			if (charging) {
				packed |= BQ27220_MSD_CHARGING_BIT;
			}
			opendisplay_ble_set_dynamic_byte(msd_idx, packed);
		}
	}
}
