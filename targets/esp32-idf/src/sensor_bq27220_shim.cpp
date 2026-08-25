/* The four names display_service.cpp calls, over shared/core/od_sensor_bq27220.h. */

#include "sensor_bq27220.h"

#include "od_config.h"
#include "od_hal_time.h"
#include "od_sensor_bq27220.h"

extern struct od_config globalConfig;

void initBq27220Sensors(void) { od_sensor_bq27220_init(&globalConfig); }
void pollBq27220ForMsd(void) { od_sensor_bq27220_poll(&globalConfig, od_hal_uptime_ms()); }
bool bq27220IsConfigured(void) { return od_sensor_bq27220_is_configured(&globalConfig); }
float bq27220BatteryVoltageVolts(void) { return od_sensor_bq27220_voltage_volts(); }
