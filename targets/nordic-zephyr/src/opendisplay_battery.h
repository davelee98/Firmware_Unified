#ifndef OPENDISPLAY_BATTERY_H
#define OPENDISPLAY_BATTERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Battery voltage measurement. Source priority mirrors the nRF52840 reference
 * readBatteryVoltageUncached(): a configured BQ27220 fuel gauge wins, otherwise
 * the SAADC on the configured battery_sense_pin. Results are cached for 30 s.
 */

/* Cached battery voltage in volts, or -1.0f if not configured / unavailable. */
float opendisplay_battery_read_voltage_volts(void);

/* Cached battery voltage in 10 mV units, clamped to 0..511 (9-bit MSD field);
 * 0 when unavailable. */
uint16_t opendisplay_battery_get_10mv(void);

#ifdef __cplusplus
}
#endif

#endif
