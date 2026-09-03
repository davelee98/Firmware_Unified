#ifndef OD_GPIO_H
#define OD_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Encode Pport.pin for configs. Pins 0..15: (port<<4)|pin.
 * Pins 16..31: 0x80 | (port<<5) | pin  (needed for LM20 D1=P1.31 etc.). */
#define OD_GPIO_CFG(port, pin)                                                 \
	(((pin) > 15) ? (uint8_t)(0x80u | (((port) & 3u) << 5) | ((pin) & 0x1Fu))  \
		      : (uint8_t)((((port) & 0x0Fu) << 4) | ((pin) & 0x0Fu)))

bool od_pin_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out);
/* Platform codecs use this to reject encoded ports absent from the selected SoC. */
bool od_gpio_port_ready(uint8_t port);
void od_gpio_configure_output(uint8_t cfg, bool initial_high);

/* Make a pad an output WITHOUT choosing its level -- ESP32's od_hal_gpio_set_mode_output(), and
 * Arduino's pinMode(OUTPUT) before it.
 *
 * THE GT911 RESET SEQUENCE NEEDS THIS AND configure_output() CANNOT SERVE IT. That sequence
 * re-asserts RST as an output while it is already high, and configure_output() must pick an
 * initial level: passing false drives RST low for the width of the call, producing a spurious
 * falling-then-rising edge that re-runs the part's address strap. Zephyr's GPIO_OUTPUT alone
 * leaves the latch untouched, which is the behaviour the sequence assumes. */
void od_gpio_set_mode_output(uint8_t cfg);
void od_gpio_configure_input(uint8_t cfg, bool pull_up, bool pull_down);
void od_gpio_write(uint8_t cfg, bool level_high);
int od_gpio_read(uint8_t cfg);
void od_gpio_park(uint8_t cfg);

/* ---- pin interrupts ----
 *
 * Modelled on ESP32's od_hal_gpio surface so a shared consumer sees one shape. This target had
 * NO GPIO interrupts at all before; the driver for adding them is the button work
 * (plans/PLAN_NORDIC_BUTTONS_2026-08-22.md B1/B2/B5 -- the ISR records the transition, per-button
 * state, and debounce moves into the ISR), which is why the `_arg` variant exists: four buttons
 * sharing one handler need their own index, and the alternative is four near-identical ISRs.
 *
 * ISR CONTEXT: THE HANDLER MAY SET A FLAG ONLY. It runs from Zephyr's GPIO callback, so it must
 * not log, take a lock, touch I2C, or call anything that can block.
 *
 * Re-attaching a pin REPLACES its handler rather than failing. The pin's input mode and pull are
 * not touched here -- configure them with od_gpio_configure_input() first.
 *
 * Return 0 on success, negative on failure. */
typedef enum {
	OD_GPIO_EDGE_RISING = 0,
	OD_GPIO_EDGE_FALLING,
	OD_GPIO_EDGE_BOTH,
} od_gpio_edge_t;

typedef void (*od_gpio_irq_fn)(void);

int od_gpio_config_irq(uint8_t cfg, od_gpio_edge_t edge, od_gpio_irq_fn handler);

/* Detaches and disables. Safe on a pin that was never attached. */
void od_gpio_clear_irq(uint8_t cfg);

/* Global interrupt lock, for a caller that must read several pins as one snapshot. */
void od_gpio_irq_lock(void);
void od_gpio_irq_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
