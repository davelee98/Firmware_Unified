#include "opendisplay_battery.h"
#include "opendisplay_ble.h"
#include "opendisplay_sensor_bq27220.h"
#include "opendisplay_sensor_npm1300.h"
#include "opendisplay_structs.h"
#include "nrf54_gpio.h"

#include <stdio.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>

/*
 * SAADC battery read.
 *
 * The 8 SAADC channels are defined in the board devicetree (channel@0..7 ->
 * NRF_SAADC_AIN0..AIN7, gain 1/4, internal reference, 12-bit, 8x oversample).
 * app.overlay exposes them through /zephyr,user io-channels so we get one
 * ADC_DT_SPEC per channel and can select at runtime by index. On nRF54L the
 * analog inputs are on P1.00..P1.07 -> AIN0..AIN7, so only pins on port 1 with
 * pin 0..7 are analog-capable; anything else is rejected with a log.
 *
 * Transfer-function parity with the reference:
 *   nRF52840 Arduino analogRead() is 10-bit with AR_DEFAULT (0.6V internal ref,
 *   1/6 gain => 3.6V full scale), so raw10 = pin_volts / 3.6 * 1024. Existing
 *   device configs calibrated voltage_scaling_factor for
 *       volts = raw10 * factor / 100000.
 *   Here the SAADC yields pin millivolts (adc_raw_to_millivolts_dt), which we
 *   convert to the same 10-bit count space before applying the formula:
 *       raw10 = pin_mv * 1024 / 3600
 *       volts = raw10 * factor / 100000
 *   so unchanged configs produce the same voltage.
 */

#define OD_BATTERY_TTL_MS 30000u
#define OD_BATTERY_SAMPLES 10

#if DT_NODE_EXISTS(DT_PATH(zephyr_user)) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#define OD_ADC_AVAILABLE 1
static const struct adc_dt_spec s_adc_specs[] = {
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 3),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 4),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 5),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 6),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 7),
};
#else
#define OD_ADC_AVAILABLE 0
#endif

/* Map a compact nRF54 pin byte to a SAADC channel index (== AIN index).
 * Returns -1 for non-analog pins. */
static int battery_pin_to_ain(uint8_t pin_cfg)
{
	uint8_t port;
	uint8_t pin;

	if (!nrf54_pin_decode(pin_cfg, &port, &pin)) {
		return -1;
	}
	if (port != 1u || pin > 7u) {
		return -1;
	}
	return (int)pin;
}

#if OD_ADC_AVAILABLE
/* Read one channel and return the pin voltage in millivolts, or -1 on error. */
static int32_t saadc_read_mv(const struct adc_dt_spec *spec)
{
	int16_t sample = 0;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	if (adc_sequence_init_dt(spec, &seq) != 0) {
		return -1;
	}
	if (adc_read(spec->dev, &seq) != 0) {
		return -1;
	}
	int32_t mv = sample;
	/* Single-ended readings can dip slightly negative near 0. */
	if (mv < 0) {
		mv = 0;
	}
	if (adc_raw_to_millivolts_dt(spec, &mv) != 0) {
		return -1;
	}
	return mv;
}
#endif

/* Uncached SAADC path. Returns volts, or -1.0f if unavailable. */
static float battery_read_saadc_volts(void)
{
	const struct GlobalConfig *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return -1.0f;
	}
	uint8_t sense_pin = cfg->power_option.battery_sense_pin;
	uint8_t enable_pin = cfg->power_option.battery_sense_enable_pin;
	uint16_t scaling = cfg->power_option.voltage_scaling_factor;

	if (sense_pin == 0xFFu) {
		return -1.0f;
	}
	int ain = battery_pin_to_ain(sense_pin);
	if (ain < 0) {
		printf("[OD] battery: sense pin 0x%02X is not SAADC-capable "
		       "(need P1.00-P1.07)\r\n", sense_pin);
		return -1.0f;
	}
#if !OD_ADC_AVAILABLE
	printf("[OD] battery: ADC not available in this build\r\n");
	return -1.0f;
#else
	const struct adc_dt_spec *spec = &s_adc_specs[ain];

	if (!adc_is_ready_dt(spec)) {
		printf("[OD] battery: ADC device not ready\r\n");
		return -1.0f;
	}
	if (adc_channel_setup_dt(spec) != 0) {
		printf("[OD] battery: adc_channel_setup failed (AIN%d)\r\n", ain);
		return -1.0f;
	}

	/* Enable the sense divider (reference drives it HIGH; battery_sense_flags
	 * ENABLE_INVERTED is not honored, matching readBatteryVoltageUncached). */
	if (enable_pin != 0xFFu) {
		nrf54_gpio_configure_output(enable_pin, true);
		k_msleep(10);
	}

	int64_t mv_sum = 0;
	int good = 0;
	for (int i = 0; i < OD_BATTERY_SAMPLES; i++) {
		int32_t mv = saadc_read_mv(spec);
		if (mv >= 0) {
			mv_sum += mv;
			good++;
		}
		k_msleep(2);
	}

	if (enable_pin != 0xFFu) {
		nrf54_gpio_write(enable_pin, false);
		nrf54_gpio_park(enable_pin);
	}

	if (good == 0) {
		return -1.0f;
	}
	int32_t avg_mv = (int32_t)(mv_sum / good);
	/* Convert to the reference's 10-bit / 3.6V full-scale count space. */
	int32_t raw10 = (avg_mv * 1024) / 3600;
	if (scaling > 0u) {
		return (float)((double)raw10 * (double)scaling / 100000.0);
	}
	return -1.0f;
#endif
}

/* Uncached: nPM1300 (LM20) / BQ27220 first, then SAADC. */
static float battery_read_uncached(void)
{
	if (opendisplay_sensor_npm1300_is_configured()) {
		float v = opendisplay_sensor_npm1300_voltage_volts();
		if (v >= 0.0f) {
			return v;
		}
	}
	if (opendisplay_sensor_bq27220_is_configured()) {
		float gauge_v = opendisplay_sensor_bq27220_voltage_volts();
		if (gauge_v >= 0.0f) {
			return gauge_v;
		}
	}
	return battery_read_saadc_volts();
}

float opendisplay_battery_read_voltage_volts(void)
{
	static uint32_t last_read_ms;
	static float cached = -1.0f;
	static bool have_reading;

	uint32_t now = k_uptime_get_32();

	if (have_reading && (now - last_read_ms) < OD_BATTERY_TTL_MS) {
		return cached;
	}
	cached = battery_read_uncached();
	last_read_ms = now;
	/* Only TTL-cache successful readings; retry soon after a miss. */
	have_reading = (cached >= 0.0f);
	return cached;
}

uint16_t opendisplay_battery_get_10mv(void)
{
	float volts = opendisplay_battery_read_voltage_volts();

	if (volts < 0.0f) {
		return 0u;
	}
	uint16_t mv = (uint16_t)(volts * 1000.0f);
	uint16_t v10 = (uint16_t)(mv / 10u);
	if (v10 > 511u) {
		v10 = 511u;
	}
	return v10;
}
