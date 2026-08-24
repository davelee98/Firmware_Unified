/* The four names opendisplay_ble.c and opendisplay_battery.c call, over the shared driver. */

#include "opendisplay_sensor_bq27220.h"

#include "od_sensor_bq27220.h"
#include "opendisplay_ble.h"

#include <zephyr/kernel.h>

void opendisplay_sensor_bq27220_init(void)
{
	od_sensor_bq27220_init(opendisplay_get_global_config());
}

void opendisplay_sensor_bq27220_poll(void)
{
	od_sensor_bq27220_poll(opendisplay_get_global_config(), k_uptime_get_32());
}

bool opendisplay_sensor_bq27220_is_configured(void)
{
	return od_sensor_bq27220_is_configured(opendisplay_get_global_config());
}

float opendisplay_sensor_bq27220_voltage_volts(void)
{
	return od_sensor_bq27220_voltage_volts();
}
