/* od_led_app — what od_led needs from a target. Three functions, and none of them is a GPIO HAL.
 *
 * Pin CONFIGURATION stays in each target's own init; this is only the level write the software
 * PWM ramp performs, plus the two accesses to the persisted mode nibble that the runner cannot
 * copy (see od_led.h).
 */

#ifndef OD_LED_APP_H
#define OD_LED_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drive one pin. `pin_cfg` is the encoded byte from the config; OD_PIN_UNUSED (0xFF) must be
 * accepted and ignored, because a pattern may name colours the board has no LED for.
 *
 * Called from the PWM ramp, which issues seven writes per brightness step -- keep it cheap and do
 * not log from it. */
void od_led_app_write(uint8_t pin_cfg, bool level_high);

/* The LIVE mode nibble of the instance's LedConfig.reserved[0], masked to 0x0F.
 *
 * Read on every service call. Anything that clears the byte -- a config write, a reload -- stops
 * the pattern, which is behaviour the wire depends on. A target that cannot supply it must not
 * fake a constant 1: that silently converts an externally stoppable pattern into one that only
 * ends on its own terms. */
uint8_t od_led_app_mode(uint8_t instance);

/* The run ended naturally or through an explicit stop. Clear the running instance's mode nibble.
 *
 * NOT called on displacement, or when the run ends because the nibble was already cleared
 * elsewhere -- there is nothing to clear, and re-clearing would race whoever cleared it. */
void od_led_app_finished(uint8_t instance);

#ifdef __cplusplus
}
#endif

#endif /* OD_LED_APP_H */
