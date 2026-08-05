#ifndef OPENDISPLAY_SENSOR_BQ27220_H
#define OPENDISPLAY_SENSOR_BQ27220_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure charger GPIO and probe a configured BQ27220 fuel gauge. */
void opendisplay_sensor_bq27220_init(void);

/* Poll the gauge (voltage + SOC), cache the voltage for the battery source,
 * and write the SOC/charging byte into the MSD dynamic region. 30 s TTL. */
void opendisplay_sensor_bq27220_poll(void);

/* True when a BQ27220 sensor is present in the config. */
bool opendisplay_sensor_bq27220_is_configured(void);

/* Last polled gauge voltage in volts, or -1.0f if unavailable. */
float opendisplay_sensor_bq27220_voltage_volts(void);

#ifdef __cplusplus
}
#endif

#endif
