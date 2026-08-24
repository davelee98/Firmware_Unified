#ifndef SENSOR_SHT40_H
#define SENSOR_SHT40_H
/* Thin names over shared/core/od_sensor_sht40.h, kept because two call sites in
 * display_service.cpp use them and renaming those is not this cutover's business. */
void initSht40Sensors(void);
void pollSht40SensorsForMsd(void);
#endif
