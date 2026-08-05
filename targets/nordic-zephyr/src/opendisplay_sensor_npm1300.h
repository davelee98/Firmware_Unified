#ifndef OPENDISPLAY_SENSOR_NPM1300_H
#define OPENDISPLAY_SENSOR_NPM1300_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void opendisplay_sensor_npm1300_init(void);
void opendisplay_sensor_npm1300_poll(void);

/* True when config has SENSOR_TYPE_NPM1300 and its data_bus resolves. */
bool opendisplay_sensor_npm1300_is_available(void);

/* True when config has SENSOR_TYPE_NPM1300. */
bool opendisplay_sensor_npm1300_is_configured(void);

/* Last polled battery voltage in volts, or -1 if unknown. */
float opendisplay_sensor_npm1300_voltage_volts(void);

/* Enter nPM1300 hibernate/ship-style low power (best-effort). */
void opendisplay_sensor_npm1300_enter_hibernate(void);

#ifdef __cplusplus
}
#endif

#endif
