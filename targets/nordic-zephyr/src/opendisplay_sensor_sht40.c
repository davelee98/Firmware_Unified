#include "opendisplay_sensor_sht40.h"
#include "opendisplay_sensor_common.h"
#include "opendisplay_ble.h"
#include "opendisplay_structs.h"

#include <stdio.h>
#include <zephyr/kernel.h>

#define SHT40_CMD_MEASURE_HIGH 0xFDu
#define SHT40_CMD_SOFT_RESET   0x94u
#define SHT40_MEASURE_DELAY_MS 12u
#define SHT40_MSD_POLL_TTL_MS  30000u

static uint8_t sht40_crc8(const uint8_t *data, uint8_t len)
{
	uint8_t crc = 0xFFu;

	for (uint8_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (uint8_t bit = 8; bit > 0; bit--) {
			if (crc & 0x80u) {
				crc = (uint8_t)((crc << 1) ^ 0x31u);
			} else {
				crc = (uint8_t)(crc << 1);
			}
		}
	}
	return crc;
}

static uint8_t sht40_addr_7bit(const struct SensorData *s)
{
	uint8_t a = s->i2c_addr_7bit;

	if (a == 0u || a == 0xFFu) {
		return 0x44u;
	}
	return a;
}

static uint8_t sht40_msd_start(const struct SensorData *s)
{
	uint8_t st = s->msd_data_start_byte;

	if (st == 0xFFu || st == 0u) {
		return 7u;
	}
	return st;
}

static bool sht40_read_measurement(struct od_i2c_bus *bus, uint8_t addr7,
				   int16_t *temp_centi, uint16_t *rh_centi)
{
	uint8_t cmd = SHT40_CMD_MEASURE_HIGH;

	if (!od_i2c_write(bus, addr7, &cmd, 1, true)) {
		return false;
	}
	k_msleep(SHT40_MEASURE_DELAY_MS);

	uint8_t b[6];

	if (!od_i2c_read(bus, addr7, b, sizeof(b))) {
		return false;
	}
	if (sht40_crc8(b, 2) != b[2] || sht40_crc8(b + 3, 2) != b[5]) {
		return false;
	}
	uint16_t raw_t = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
	uint16_t raw_rh = (uint16_t)(((uint16_t)b[3] << 8) | b[4]);
	float tc = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
	float rh = -6.0f + 125.0f * ((float)raw_rh / 65535.0f);

	if (rh < 0.0f) {
		rh = 0.0f;
	}
	if (rh > 100.0f) {
		rh = 100.0f;
	}
	*temp_centi = (int16_t)(tc * 100.0f);
	*rh_centi = (uint16_t)(rh * 100.0f);
	return true;
}

static bool read_sht40_sample(const struct SensorData *s, int16_t *temp_centi,
			      uint16_t *rh_centi)
{
	struct od_i2c_bus bus;

	if (!od_sensor_bus_for(s->bus_id, &bus)) {
		return false;
	}
	const uint8_t candidates[] = {sht40_addr_7bit(s), 0x44u, 0x45u};

	for (uint8_t i = 0; i < sizeof(candidates); i++) {
		bool dup = false;

		for (uint8_t j = 0; j < i; j++) {
			if (candidates[j] == candidates[i]) {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		if (sht40_read_measurement(&bus, candidates[i], temp_centi, rh_centi)) {
			return true;
		}
	}
	return false;
}

static int round_centi_to_deci(int16_t c)
{
	if (c >= 0) {
		return (int)((c + 5) / 10);
	}
	return (int)((c - 5) / 10);
}

/* MSD (3 bytes LE): v = rh_deci | (tu << 10); rh_deci 0..1000 (0.1% steps);
 * tu = temp(0.1C) + 400. Decode matches opendisplay-msd.js decodeSht40Three(). */
static void write_sht40_msd(uint8_t start, int16_t temp_centi, uint16_t rh_centi)
{
	int t_deci = round_centi_to_deci(temp_centi);

	if (t_deci < -400) {
		t_deci = -400;
	}
	if (t_deci > 1250) {
		t_deci = 1250;
	}
	uint32_t tu = (uint32_t)(t_deci + 400);
	uint32_t rh_d = ((uint32_t)rh_centi + 5u) / 10u;

	if (rh_d > 1000u) {
		rh_d = 1000u;
	}
	uint32_t v = (rh_d & 0x3FFu) | (tu << 10);

	opendisplay_ble_set_dynamic_byte(start, (uint8_t)(v & 0xFFu));
	opendisplay_ble_set_dynamic_byte((uint8_t)(start + 1u), (uint8_t)((v >> 8) & 0xFFu));
	opendisplay_ble_set_dynamic_byte((uint8_t)(start + 2u), (uint8_t)((v >> 16) & 0xFFu));
	if ((uint16_t)start + 3u < 11u) {
		opendisplay_ble_set_dynamic_byte((uint8_t)(start + 3u), 0u);
	}
}

static void write_sht40_invalid(uint8_t start)
{
	opendisplay_ble_set_dynamic_byte(start, 0xFFu);
	opendisplay_ble_set_dynamic_byte((uint8_t)(start + 1u), 0xFFu);
	opendisplay_ble_set_dynamic_byte((uint8_t)(start + 2u), 0xFFu);
	if ((uint16_t)start + 3u < 11u) {
		opendisplay_ble_set_dynamic_byte((uint8_t)(start + 3u), 0u);
	}
}

void opendisplay_sensor_sht40_init(void)
{
	const struct GlobalConfig *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return;
	}
	for (uint8_t i = 0; i < cfg->sensor_count; i++) {
		const struct SensorData *s = &cfg->sensors[i];

		if (s->sensor_type != SENSOR_TYPE_SHT40) {
			continue;
		}
		struct od_i2c_bus bus;

		if (!od_sensor_bus_for(s->bus_id, &bus)) {
			continue;
		}
		uint8_t cmd = SHT40_CMD_SOFT_RESET;

		if (!od_i2c_write(&bus, sht40_addr_7bit(s), &cmd, 1, true)) {
			(void)od_i2c_write(&bus, 0x44u, &cmd, 1, true);
			(void)od_i2c_write(&bus, 0x45u, &cmd, 1, true);
		}
		k_msleep(2);
	}
}

void opendisplay_sensor_sht40_poll(void)
{
	static uint32_t last_poll_ms;
	static bool have_polled;
	static bool logged_fail;

	const struct GlobalConfig *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return;
	}
	uint32_t now = k_uptime_get_32();

	if (have_polled && (now - last_poll_ms) < SHT40_MSD_POLL_TTL_MS) {
		return;
	}
	last_poll_ms = now;
	have_polled = true;

	for (uint8_t i = 0; i < cfg->sensor_count; i++) {
		const struct SensorData *s = &cfg->sensors[i];

		if (s->sensor_type != SENSOR_TYPE_SHT40) {
			continue;
		}
		uint8_t start = sht40_msd_start(s);

		if (start > 8u) {
			continue;
		}
		int16_t tc = 0;
		uint16_t rhc = 0;

		if (!read_sht40_sample(s, &tc, &rhc)) {
			if (!logged_fail) {
				printf("[OD] SHT40: read failed (bus %u)\r\n",
				       (unsigned)s->bus_id);
				logged_fail = true;
			}
			write_sht40_invalid(start);
			continue;
		}
		logged_fail = false;
		write_sht40_msd(start, tc, rhc);
	}
}
