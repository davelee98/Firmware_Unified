/* Nordic's half of od_sensor_app.h. */

#include "od_sensor_app.h"

#include "opendisplay_ble.h"

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
