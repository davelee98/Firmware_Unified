/* Shared BQ27220 fuel-gauge driver: device policy, not just conversion.
 *
 * FIRST MATCH ONLY, unlike SHT40. Both ports return the first BQ27220 entry in the config and
 * ignore any others, and the promotion must not silently make it multi-instance -- a second
 * gauge would need somewhere of its own in the MSD and a rule for which one
 * od_sensor_bq27220_voltage_volts() means. That is why the seams below take no instance index
 * while od_sensor_app_msd_write() does.
 *
 * It owns the config walk, the default address and initial probe, the register selectors and
 * their widths, the poll TTL and the have-polled latch, the cached voltage and what a failed or
 * implausible read does to it, the SOC clamp, and the MSD packing. The bus is
 * shared/hal/od_hal_i2c.h; the charger GPIO and the MSD block are od_sensor_app.h.
 */

#ifndef OD_SENSOR_BQ27220_H
#define OD_SENSOR_BQ27220_H

#include "od_config.h"

#ifdef __cplusplus
extern "C" {
#endif

bool od_sensor_bq27220_is_configured(const struct od_config *cfg);

/* Establish the charger GPIO and probe the gauge once. */
void od_sensor_bq27220_init(const struct od_config *cfg);

/* `now_ms` is the caller's clock, passed explicitly -- shared/core does not sample the ambient
 * time HAL. See od_sensor_sht40.h for why. */
void od_sensor_bq27220_poll(const struct od_config *cfg, uint32_t now_ms);

/* Last good pack voltage, or -1.0 when the gauge has not answered plausibly. The cache survives
 * a failed poll only in the sense that it is INVALIDATED by one: a stale voltage presented as
 * current is worse than none. */
float od_sensor_bq27220_voltage_volts(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_SENSOR_BQ27220_H */
