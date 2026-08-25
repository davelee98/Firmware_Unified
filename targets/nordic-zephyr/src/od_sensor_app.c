/* Nordic's half of od_sensor_app.h. */

#include "od_sensor_app.h"

#include "opendisplay_ble.h"
#include "od_gpio.h"
#include "od_runtime_types.h"

#include <zephyr/kernel.h>

void od_sensor_app_delay_ms(uint16_t delay_ms)
{
	k_msleep((int32_t)delay_ms);
}

void od_sensor_app_msd_write(uint8_t index, uint8_t value)
{
	opendisplay_ble_set_dynamic_byte(index, value);
}

void od_sensor_app_bus_recover(uint8_t bus_id)
{
	/* Nothing to tear down: this target re-initialises the bus inside every operation, so a
	 * retry already gets a fresh bus. What is left is the settle the authority waits out. */
	(void)bus_id;
	k_msleep(2);
}

static bool valid_pin(uint8_t pin)
{
	return pin != 0u && pin != 0xFFu;
}

bool od_sensor_app_bq_enable_drive(bool level_high)
{
	const struct od_config *cfg = opendisplay_get_global_config();
	uint8_t en;
	uint8_t st;

	if (cfg == NULL) {
		return false;
	}
	en = cfg->power_option.charge_enable_pin;
	st = cfg->power_option.charge_state_pin;

	if (valid_pin(en)) {
		od_gpio_configure_output(en, level_high);
	}
	if (valid_pin(st)) {
		od_gpio_configure_input(st, true, false);
	}
	/* An absent enable pin is a successful no-op. */
	return true;
}

bool od_sensor_app_bq_state_level(bool *level_high)
{
	const struct od_config *cfg = opendisplay_get_global_config();
	uint8_t st;
	int level;

	if (cfg == NULL || level_high == NULL) {
		return false;
	}
	st = cfg->power_option.charge_state_pin;
	if (!valid_pin(st)) {
		return false;      /* no state pin: UNKNOWN, not "not charging" */
	}
	level = od_gpio_read(st);
	if (level < 0) {
		return false;      /* a failed read is unknown too, not a level */
	}
	*level_high = (level != 0);
	return true;
}
