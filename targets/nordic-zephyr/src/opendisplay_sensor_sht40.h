#ifndef OPENDISPLAY_SENSOR_SHT40_H
#define OPENDISPLAY_SENSOR_SHT40_H

#ifdef __cplusplus
extern "C" {
#endif

/* Soft-reset any configured SHT40 sensors (call once after config load). */
void opendisplay_sensor_sht40_init(void);

/* Poll configured SHT40 sensors and write their 3-byte block into the MSD
 * dynamic-return region. Rate-limited to once per 30 s. */
void opendisplay_sensor_sht40_poll(void);

#ifdef __cplusplus
}
#endif

#endif
