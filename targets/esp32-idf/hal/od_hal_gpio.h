/* od_hal_gpio -- pin configuration and level access.
 *
 * Signatures follow docs/SHARED_API_DESIGN.md § od_hal_gpio. The portability decision that
 * matters is already made there and is honoured here: **`cfg` is an opaque target-encoded pin
 * byte**, decoded inside the HAL and never by the caller. nRF54 encodes (port<<4)|pin, Silabs
 * encodes 0xPN, and on ESP32 the encoding is the identity -- cfg IS the GPIO number. That is
 * why the same host-written config blob drives three different pin encodings unchanged, and it
 * is the reason callers must not do arithmetic on a cfg byte.
 *
 * Functions are declared as they gain callers, not up front. park() and decode() are in the
 * design doc and are still absent; config_irq() arrived in phase C step 6 with touch_input.cpp,
 * its first consumer.
 *
 * Behaviour is the shim's, exactly: compat/arduino_compat.h's pinMode/digitalWrite/digitalRead
 * were already thin wrappers over gpio_config/gpio_set_level/gpio_get_level with a validity
 * guard, and those bodies moved here unchanged. The Arduino names leave; the register writes do
 * not change.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The codebase's "pin not fitted" sentinel, as it arrives from a host config packet. */
#define OD_PIN_UNUSED 0xFFu

/* Push-pull output, then driven to `initial_high`.
 *
 * Two calls, in that order, because gpio_config() does NOT set the output latch -- the pad
 * keeps whatever level it last held. Merging them into one "configure at level" call would
 * change the glitch behaviour on a pin that was already driven the other way, and this file's
 * first consumer is a power latch where that glitch cuts the rail. */
void od_hal_gpio_config_output(uint8_t cfg, bool initial_high);

/* Input, with at most one pull enabled. Passing both is a caller bug on ESP32 (the pad has one
 * pull each way and enabling both is a contradiction, not a compromise); the pull-up wins, so
 * the result is defined rather than silently chip-specific. */
void od_hal_gpio_config_input(uint8_t cfg, bool pull_up, bool pull_down);

/* Mode only: make the pad an output WITHOUT changing its level. Separate from
 * config_output() because pinMode() and digitalWrite() are separable operations and at least
 * one caller depends on the separation: the GT911 hardware reset drives INT and RST in a
 * specific interleaved ORDER, and the level of INT at RST's rising edge is what selects the
 * controller's I2C address (0x14 vs 0x5D). Collapsing its pinMode/digitalWrite pairs would
 * reorder that sequence. Prefer config_output() everywhere else -- it is one call and it cannot
 * leave a pad driving a stale level. */
void od_hal_gpio_set_mode_output(uint8_t cfg);

void od_hal_gpio_write(uint8_t cfg, bool level_high);

/* 0 or 1. An invalid or unfitted pin reads 0 -- the same answer the shim gave, and the same one
 * Arduino-ESP32 gives, so no call site's sense of "not pressed" changes. */
int od_hal_gpio_read(uint8_t cfg);

/* Edge to interrupt on. NAMED, unlike docs/SHARED_API_DESIGN.md's config_irq(), which is
 * specified as edge-both with no mode argument. That is too narrow for the first real caller:
 * the GT911 touch controller asserts INT active-low and is attached FALLING, and an edge-both
 * attachment would fire a spurious event on every release. Widened deliberately -- see
 * compat/SHIM_BUDGET, step 6. */
typedef enum {
    OD_GPIO_EDGE_RISING = 0,
    OD_GPIO_EDGE_FALLING,
    OD_GPIO_EDGE_BOTH,
} od_hal_gpio_edge_t;

/* ISR CONTEXT: THE HANDLER MAY SET A FLAG ONLY. It runs from the shared GPIO ISR, so it must
 * not log, take a lock, touch I2C, or call anything that can block -- a rule the design doc
 * states and the existing callers already honour (touch_input's four ISRs each set one bit).
 *
 * Idempotent: the ISR service is installed once, and re-attaching a pin REPLACES its handler
 * rather than failing, which is what the callers assume. The pin's input mode and pull are not
 * touched here -- configure them with od_hal_gpio_config_input() first, exactly as the Arduino
 * call sites did.
 *
 * Returns 0 on success, negative on failure. */
typedef void (*od_hal_gpio_irq_fn)(void);

int  od_hal_gpio_config_irq(uint8_t cfg, od_hal_gpio_edge_t edge, od_hal_gpio_irq_fn handler);

/* Detaches and disables. Safe on a pin that was never attached. */
void od_hal_gpio_clear_irq(uint8_t cfg);

/* Global interrupt lock. portDISABLE_INTERRUPTS is PER-CORE on ESP32, which is not the same
 * guarantee Arduino's noInterrupts() implied -- these call sites need auditing when the code
 * they guard moves to shared/core, which has no global-disable primitive at all and will need
 * an explicit irq-lock in the HAL (DESIGN_REVIEW § "Big-picture soundness"). Carried across
 * unchanged for now so the semantics do not change silently inside a shim-removal commit. */
void od_hal_gpio_irq_lock(void);
void od_hal_gpio_irq_unlock(void);

#ifdef __cplusplus
}
#endif
