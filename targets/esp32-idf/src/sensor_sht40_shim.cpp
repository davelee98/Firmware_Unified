/* The two names display_service.cpp calls, over the shared driver. */

#include "sensor_sht40.h"

#include "od_config.h"
#include "od_sensor_sht40.h"
#include "od_hal_time.h"

extern struct od_config globalConfig;

void initSht40Sensors(void) { od_sensor_sht40_init(&globalConfig); }
void pollSht40SensorsForMsd(void) { od_sensor_sht40_poll(&globalConfig, od_hal_uptime_ms()); }
