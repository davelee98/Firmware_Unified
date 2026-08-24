/* od_led — the LED_ACTIVATE (0x0073) pattern runner, shared by every target.
 *
 * The machine NEVER SLEEPS. od_led_service() advances at most one yield-requiring action and
 * returns how long until it wants the next call, so each target arms the scheduler it already has
 * -- k_timer on Zephyr, sl_sleeptimer on the BG22 superloop, a polled comparison on ESP32. A
 * shared runner that called "sleep" would be wrong on all three.
 *
 * Two properties are load-bearing and are pinned by tests/host/led_test.c:
 *
 *   - EVERY step yields. A pattern whose delays are all zero must still return, or a superloop
 *     with no watchdog spins forever on a frame any client can send.
 *   - The mode nibble is read LIVE, not copied. All three donors re-check it on every step and
 *     stop when it is cleared, so a config write halts a running pattern. Copying it in at
 *     activate would silently drop that.
 */

#ifndef OD_LED_H
#define OD_LED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned by od_led_service() when nothing is pending; the caller need not schedule. */
#define OD_LED_IDLE        0xFFFFFFFFu

/* The LED_ACTIVATE payload the runner parses, as stored in LedConfig.reserved[]. */
#define OD_LED_PATTERN_LEN 12u

/* Floor on any step that would otherwise re-enter the phase loop with no wait. */
#define OD_LED_MIN_STEP_DELAY_MS 1u

/* Encoded pin bytes and inversion flags for one instance. The machine keeps a copy: it must not
 * hold a pointer into the config, or a reload mutates a running pattern underneath it. */
struct od_led_pins {
    uint8_t r;       /* encoded pin byte; OD_PIN_UNUSED (0xFF) = absent */
    uint8_t g;
    uint8_t b;
    uint8_t flags;   /* bits 0..2: invert red / green / blue */
};

/* One flash, outside any pattern: the same software PWM the runner uses, run once and returned
 * from. For boot/diagnostic blinks that do not start a pattern -- it touches no runner state, so
 * it neither disturbs nor is disturbed by a running one.
 *
 * `brightness` is the decoded 1..16 value, not the encoded nibble. */
void od_led_flash_once(const struct od_led_pins *pins, uint8_t color, uint8_t brightness);

/* Begin `pattern` on `instance`. Returns 0 when accepted, 2 when the payload's mode nibble is not
 * "run". The adapter translates that deployed stop-current request to wire success; when no run
 * is active, it must leave the selected inactive instance's non-run nibble alone. `now_ms` seeds
 * the deadline; the first service call may be immediate.
 *
 * Displacing a running pattern parks the OUTGOING instance's LEDs but leaves its mode nibble
 * alone, which is the authority's behaviour -- so re-activating the same instance restarts it
 * rather than stopping it. */
int od_led_activate(uint8_t instance, const struct od_led_pins *pins,
                    const uint8_t pattern[OD_LED_PATTERN_LEN], uint32_t now_ms);

/* Stop. Returns 0 when stopped or already idle, 2 when `instance_given` and a different instance
 * owns the run -- matching the deployed LED_STOP contract.
 *
 * A stop that actually halts a run reports od_led_app_finished() for the RUNNING instance, which
 * the caller may not know: LED_STOP can arrive with no instance byte. */
int od_led_stop(uint8_t instance, bool instance_given);

/* Advance the pattern. Returns milliseconds until the next call is wanted, or OD_LED_IDLE.
 *
 * The delay is RELATIVE so no caller has to reason about the 32-bit millisecond wrap. Internally
 * the deadline is absolute and compared wrap-safely, which makes an early call harmless: it
 * returns the remaining delay and advances nothing. A late call advances exactly one step, so a
 * pattern slips rather than fast-forwarding -- which is what all three donors do.
 *
 * NEVER RETURNS 0. Every scheduled wait is either OD_LED_MIN_STEP_DELAY_MS or a non-zero multiple
 * of 100 ms, so an adapter needs no "due immediately" branch and should not carry one. */
uint32_t od_led_service(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* OD_LED_H */
