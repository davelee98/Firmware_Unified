/* od_sensor_app -- the few things a shared sensor driver cannot do for itself.
 *
 * NOT a general time, GPIO or advertising HAL. Each entry here exists because one shared driver
 * needs it and no portable contract already provides it; anything broader belongs in shared/hal.
 * A target that takes no sensor driver implements none of these, which is why they are an APP
 * seam rather than a HAL -- BG22 has no sensors and grows no dummy functions.
 */

#ifndef OD_SENSOR_APP_H
#define OD_SENSOR_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A YIELDING wait, between two completed I2C transactions. The SHT40 needs the conversion time
 * before it will answer, and busy-waiting 12 ms on a target with a scheduler is 12 ms nothing
 * else runs. Nordic uses k_msleep; ESP32 uses its target-private bounded sleep. */
void od_sensor_app_delay_ms(uint16_t delay_ms);

/* Write one byte of the advertisement's dynamic block. The block itself is the target's -- ESP32
 * keeps an array, Nordic goes through its BLE layer -- and the index has already been bounds
 * checked by the caller against the 11-byte window. */
void od_sensor_app_msd_write(uint8_t index, uint8_t value);

/* Recover a bus between retry passes, then settle.
 *
 * The authority tears the bus down and brings it back up before its second attempt, which on a
 * target that CACHES a live bus is the difference between retrying and repeating. Nordic
 * re-initialises per operation and has nothing to tear down, so its implementation is the
 * settle alone. Called only on the retry path, never in the ordinary case. */
void od_sensor_app_bus_recover(uint8_t bus_id);

/* THESE TWO TAKE AND RETURN ELECTRICAL LEVELS, NOT MEANINGS. Deciding what a level means is
 * config policy -- it reads OD_CHARGER_FLAG_* out of struct od_config -- and policy belongs in
 * the shared driver. Both ports previously interpreted the flags themselves, and both got the
 * charge-state one backwards while getting the charge-enable one right; a duplicated decision is
 * how one copy drifts from its neighbour. See DIVERGENCE_MATRIX 21. */

/* Establish the charger GPIOs, driving the enable pin at the given LEVEL.
 *
 * Also configures the charge-state pin as an input, which is why this runs even on a board with
 * no enable pin. Configure the output and THEN drive the level, the order both ports used;
 * reversing it glitches the rail. An absent enable pin is a successful no-op, not a failure --
 * plenty of boards have no software charge control. */
bool od_sensor_app_bq_enable_drive(bool level_high);

/* Raw charge-state pin level, TRI-STATE by return value: false means "no state pin, or the read
 * failed, so unknown", and only then is *level_high untouched. A board with no state pin is not
 * "not charging", and collapsing the two would advertise a definite answer nobody measured. */
bool od_sensor_app_bq_state_level(bool *level_high);

#ifdef __cplusplus
}
#endif

#endif /* OD_SENSOR_APP_H */
