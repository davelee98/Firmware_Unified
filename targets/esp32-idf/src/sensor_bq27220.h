#ifndef SENSOR_BQ27220_H
#define SENSOR_BQ27220_H

/* Thin names over shared/core/od_sensor_bq27220.h.
 *
 * initChargerGpio() is NOT here any more: the charger GPIO is established inside
 * od_sensor_bq27220_init() via the od_sensor_app seam, and it had no caller outside the driver
 * on either target. Leaving the declaration would have left a link error waiting for whoever
 * called it first. */
void initBq27220Sensors(void);
void pollBq27220ForMsd(void);
bool bq27220IsConfigured(void);
float bq27220BatteryVoltageVolts(void);

#endif
