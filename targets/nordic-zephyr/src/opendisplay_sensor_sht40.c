/* The two names opendisplay_ble.c calls, over shared/core/od_sensor_sht40.c. */

#include "opendisplay_sensor_sht40.h"

#include "od_sensor_sht40.h"
#include "opendisplay_ble.h"

#include <zephyr/kernel.h>

void opendisplay_sensor_sht40_init(void)
{
	od_sensor_sht40_init(opendisplay_get_global_config());
}

void opendisplay_sensor_sht40_poll(void)
{
	od_sensor_sht40_poll(opendisplay_get_global_config(), k_uptime_get_32());
}
