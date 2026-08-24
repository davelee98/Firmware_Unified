/* Shared GT911 touch driver.
 *
 * ############################################################################################
 * DRAFT -- DO NOT MERGE. This failed review on 2026-08-24 and is NOT a faithful promotion. It is
 * missing the coordinate map entirely, never asserts enable_pin, gutted resume, and gets the
 * address cascade wrong; its poll() API cannot express the donors' per-controller cadence,
 * IRQ-selected polling or publish-on-change. The findings are the specification for the next
 * attempt -- plans/PLAN_SENSOR_SEAM_2026-08-23.md step 8. No target adopts it.
 * ############################################################################################
 *
 * It owns the config walk, address resolution and the reset dance that selects the address, the
 * product-ID probe including the 16-bit register BYTE ORDER fallback, BOTH I2C read framings,
 * the retry and failure-disable policy, the status/contact decode, and the MSD packing. The bus
 * is shared/hal/od_hal_i2c.h; GPIO and delays are od_touch_app.h.
 *
 * BOTH READ FRAMINGS ARE KEPT, and that is the point of promoting this rather than either port.
 * Real GT911 clones differ: some answer a repeated START, some need STOP-then-START. ESP32 tried
 * both; Nordic only ever tried the first, so it could not talk to half the parts. Each retry
 * attempt tries repeated START, then STOP-then-START, before backing off.
 *
 * ONLY ESP32 CAN RUN THIS. No Nordic board in this fleet has a touch controller, so the Nordic
 * side is compiled and never executed -- see docs/HARDWARE_VERIFICATION_CHECKLIST.md. Do not cite
 * a Nordic build as evidence for any behaviour here.
 */

#ifndef OD_TOUCH_GT911_H
#define OD_TOUCH_GT911_H

#include "od_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OD_TOUCH_MAX_CONTROLLERS 4u

/* Reset, resolve an address, probe the product ID and prepare each configured controller. */
void od_touch_gt911_init(const struct od_config *cfg);

/* Sample every ready controller and pack contacts into the MSD. `now_ms` is the caller's clock,
 * passed explicitly -- shared/core does not sample the ambient time HAL. */
void od_touch_gt911_poll(const struct od_config *cfg, uint32_t now_ms);

/* True when `pin` is a configured controller's interrupt pin, so a target's ISR plumbing can ask
 * whether an edge belongs to touch without duplicating the config walk. */
bool od_touch_gt911_is_int_pin(const struct od_config *cfg, uint8_t pin);

/* Re-establish a controller after the panel has had the bus, without a full reset. */
void od_touch_gt911_resume(const struct od_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* OD_TOUCH_GT911_H */
