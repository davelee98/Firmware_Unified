#ifndef NRF54_GPIO_H
#define NRF54_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRF54_GPIO_PIN_UNUSED 0xFFu

/* Encode Pport.pin for configs. Pins 0..15: (port<<4)|pin.
 * Pins 16..31: 0x80 | (port<<5) | pin  (needed for LM20 D1=P1.31 etc.). */
#define NRF54_GPIO_CFG(port, pin)                                              \
	(((pin) > 15) ? (uint8_t)(0x80u | (((port) & 3u) << 5) | ((pin) & 0x1Fu))  \
		      : (uint8_t)((((port) & 0x0Fu) << 4) | ((pin) & 0x0Fu)))

/* Simple parameterless callback invoked from GPIO interrupt (ISR) context. The
 * handler MUST NOT do I2C/BLE/blocking work; it should only set a flag that the
 * main loop consumes. */
typedef void (*nrf54_gpio_irq_handler_t)(void);

bool nrf54_pin_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out);
void nrf54_gpio_configure_output(uint8_t cfg, bool initial_high);
void nrf54_gpio_configure_input(uint8_t cfg, bool pull_up, bool pull_down);
/* Enable a both-edges GPIO interrupt on the pin, invoking handler in ISR
 * context. Returns 0 on success, negative on error. */
int nrf54_gpio_configure_interrupt(uint8_t cfg, nrf54_gpio_irq_handler_t handler);
void nrf54_gpio_write(uint8_t cfg, bool level_high);
int nrf54_gpio_read(uint8_t cfg);
void nrf54_gpio_park(uint8_t cfg);

#ifdef __cplusplus
}
#endif

#endif
