/* Shared GT911 touch driver.
 *
 * Owns the config walk, the reset dance that selects the I2C address, the product-ID probe with
 * its register byte-order fallback, both read framings, the retry and failure-disable policy, the
 * status/contact decode, the COORDINATE MAP, the per-controller cadence, and the MSD packing.
 * The bus is shared/hal/od_hal_i2c.h; GPIO and delays are od_touch_app.h.
 *
 * A SCHEDULED MACHINE, NOT A POLL LOOP. Every entry point returns the milliseconds until it
 * next wants to run, and takes `now_ms` rather than sampling a clock -- shared/core does not
 * touch the ambient time HAL. This is the od_led / od_buzzer idiom, and it is what lets one
 * driver serve a FreeRTOS loop, a Zephyr work queue and a superloop without any of them leaking
 * in. The first attempt at this driver exposed poll(cfg, now) and could not express per-controller
 * intervals, IRQ-selected reads, the failure backoff or publish-on-change; that is why it was
 * rejected rather than patched.
 *
 * ONLY ESP32 CAN RUN THIS. No board in this fleet has a touch controller on any other target, so
 * elsewhere it is compiled and never executed -- docs/HARDWARE_VERIFICATION_CHECKLIST.md. Do not
 * cite a build as evidence for any behaviour here.
 *
 * THE WIRE FORMAT IS FROZEN. The canonical header names the 5-byte block and bounds its offset,
 * but the layout INSIDE those bytes is defined only by a comment in the donor firmware, which
 * py-opendisplay, the JavaScript decoder and the iOS app each implement independently. There is
 * no version field, so a packing change breaks every deployed host silently. See
 * plans/PLAN_SENSOR_SEAM_2026-08-23.md 8.1, and tests/host/touch_gt911_test.c, whose expected
 * bytes were written from that comment before this file existed.
 */

#ifndef OD_TOUCH_GT911_H
#define OD_TOUCH_GT911_H

#include "od_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One bit per controller in the IRQ mask; bounded by the config's own touch array. */
#define OD_TOUCH_MAX_CONTROLLERS OD_CONFIG_MAX_TOUCH

/* What a fully idle machine asks for. Not "never": a controller can come back through the timed
 * poll after a failure streak, so the machine must keep breathing. */
#define OD_TOUCH_IDLE_MS 1000u

/* Reset, resolve an address, probe the product ID and prepare every configured controller.
 * Returns the delay until the first service() call is wanted. */
uint32_t od_touch_gt911_init(const struct od_config *cfg, uint32_t now_ms);

/* Service every controller that is due, and publish any changed sample.
 *
 * `irq_mask` carries latched FALLING edges, bit per controller index, as the target's ISR set
 * them. `*consumed_out` receives the bits this call acted on, so the TARGET clears them under its
 * own interrupt lock -- shared/ has no lock to take and must not invent one. Pass 0 and NULL on a
 * target with no interrupt wiring.
 *
 * Returns the delay until the next call is wanted. */
uint32_t od_touch_gt911_service(const struct od_config *cfg, uint32_t now_ms,
                                uint8_t irq_mask, uint8_t *consumed_out);

/* Suspend polling across a panel refresh, which contends for the bus. Nestable: the matching
 * resume() count must be reached before any controller is touched again. */
void od_touch_gt911_suspend(void);

/* Undo one suspend(); on the last one, re-establish every controller. Probes the product ID at
 * the retained address first and only falls back to a full reset and re-resolve, because a
 * working controller does not need its address re-selected. Returns the next-service delay. */
uint32_t od_touch_gt911_resume(const struct od_config *cfg, uint32_t now_ms);

/* Collapse the suspend count and resume now, whatever its depth. Idempotent. */
uint32_t od_touch_gt911_force_resume(const struct od_config *cfg, uint32_t now_ms);

/* True when `pin` is a configured controller's interrupt pin, so a target's ISR plumbing can ask
 * whether an edge belongs to touch without duplicating the config walk. */
bool od_touch_gt911_is_int_pin(const struct od_config *cfg, uint8_t pin);

/* Test and diagnostic accessors -- the resolved address, or 0 when the controller is not up. */
uint8_t od_touch_gt911_address(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif /* OD_TOUCH_GT911_H */
