/* od_touch_app -- what a shared GT911 driver cannot do for itself.
 *
 * GPIO, the reset-sequence delays, and reaching this target's advertisement. There is no shared
 * GPIO HAL and inventing one to serve a single driver would be a larger change than the promotion
 * it exists for -- ESP32 calls od_hal_gpio, Nordic calls od_gpio, and this seam is the one place
 * that difference is spelled out.
 *
 * WHAT IS *NOT* HERE IS THE POINT. Cadence, the coordinate map, publish-on-change, the failure
 * backoff and the address cascade were all proposed as seam entries during design and are all
 * policy, so they live in od_touch_gt911.c. The map especially: both donors apply it before
 * caching and before packing, so a byte-write seam sees mapped pixels or the driver is wrong.
 *
 * THE RESET SEQUENCE IS WHY set_mode_output AND write ARE SEPARATE. Both pads are made outputs
 * BEFORE either is driven, and INT's level at RST's rising edge selects the controller's I2C
 * address (GT911 Programming Guide Rev.10 4.1-4.2). A combined "configure as output at this
 * level" call would reorder that and change which address the part answers on.
 */

#ifndef OD_TOUCH_APP_H
#define OD_TOUCH_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Yielding where the target has a scheduler; the microsecond one is a busy wait by nature. */
void od_touch_app_delay_ms(uint16_t ms);
void od_touch_app_delay_us(uint32_t us);

void od_touch_app_gpio_set_mode_output(uint8_t pin);
void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up);
void od_touch_app_gpio_write(uint8_t pin, bool level_high);

/* < 0 when the pin cannot produce a level, so "unreadable" stays distinct from LOW. The charger
 * seam learned this the hard way: a reader that folds failure to 0 makes an unusable pin
 * indistinguishable from an asserted one (DIVERGENCE_MATRIX 21). */
int od_touch_app_gpio_read(uint8_t pin);

/* Attach a FALLING-edge interrupt that sets bit `idx` of the mask the target hands to
 * od_touch_gt911_service(). False means the attach failed and this controller is poll-only --
 * not an error, and the driver carries on.
 *
 * FALLING, not edge-both: GT911 asserts INT active-low, so edge-both raises a spurious event on
 * every release. Note the edge is a property of the LOADED CONFIG, not of the part: register
 * 0x804D bits 1..0 select rising, falling, or either level. Every panel in this fleet is
 * falling-edge; a panel configured otherwise works in polling and not on interrupts. */
bool od_touch_app_gpio_attach_int(uint8_t idx, uint8_t pin);
void od_touch_app_gpio_detach_int(uint8_t pin);

/* Prepare the bus for a transaction. ESP32 caches one live IDF bus and must re-select it;
 * Nordic re-initialises inside every operation and returns true. False refuses the poll. */
bool od_touch_app_bus_prepare(uint8_t bus_id);

/* Drop any cached bus state, before the driver re-establishes controllers after a panel refresh.
 *
 * BEHIND THE SEAM RATHER THAN IN THE CALLER, because only the driver knows whether a resume will
 * actually act -- it is nestable, and an unmatched resume must leave everything alone. A caller
 * that invalidated first would tear down a live bus on every no-op teardown. Nordic re-initialises
 * per operation and implements this empty. */
void od_touch_app_bus_invalidate(void);

/* One byte of the advertisement's dynamic block. Bounds are the driver's. */
void od_touch_app_msd_write(uint8_t index, uint8_t value);

/* Republish the advertisement, after a changed sample has been written.
 *
 * ORDER IS LOAD-BEARING and the driver owns it: od_adv_app_boost() is called BEFORE this,
 * because on a target with advertising-interval states the publish is what selects the interval,
 * so a boost requested afterwards lands too late for the packet it exists for. */
void od_touch_app_msd_publish(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_TOUCH_APP_H */
