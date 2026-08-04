/* od_hal_gpio -- pin configuration and level access.
 *
 * Signatures follow docs/SHARED_API_DESIGN.md § od_hal_gpio. The portability decision that
 * matters is already made there and is honoured here: **`cfg` is an opaque target-encoded pin
 * byte**, decoded inside the HAL and never by the caller. nRF54 encodes (port<<4)|pin, Silabs
 * encodes 0xPN, and on ESP32 the encoding is the identity -- cfg IS the GPIO number. That is
 * why the same host-written config blob drives three different pin encodings unchanged, and it
 * is the reason callers must not do arithmetic on a cfg byte.
 *
 * Only the four functions with callers today are declared. config_irq(), park() and decode()
 * are in the design doc and land with their first consumer (device_control.cpp needs the IRQ
 * one). Declaring them now would be a contract this file cannot be checked against.
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

void od_hal_gpio_write(uint8_t cfg, bool level_high);

/* 0 or 1. An invalid or unfitted pin reads 0 -- the same answer the shim gave, and the same one
 * Arduino-ESP32 gives, so no call site's sense of "not pressed" changes. */
int od_hal_gpio_read(uint8_t cfg);

#ifdef __cplusplus
}
#endif
