#ifndef OD_GPIO_H
#define OD_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_GPIO_PIN_UNUSED 0xFFu

/* Encode Pport.pin for configs. Pins 0..15: (port<<4)|pin.
 * Pins 16..31: 0x80 | (port<<5) | pin  (needed for LM20 D1=P1.31 etc.). */
#define OD_GPIO_CFG(port, pin)                                                 \
	(((pin) > 15) ? (uint8_t)(0x80u | (((port) & 3u) << 5) | ((pin) & 0x1Fu))  \
		      : (uint8_t)((((port) & 0x0Fu) << 4) | ((pin) & 0x0Fu)))

bool od_pin_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out);
/* Platform codecs use this to reject encoded ports absent from the selected SoC. */
bool od_gpio_port_ready(uint8_t port);
void od_gpio_configure_output(uint8_t cfg, bool initial_high);
void od_gpio_configure_input(uint8_t cfg, bool pull_up, bool pull_down);
void od_gpio_write(uint8_t cfg, bool level_high);
int od_gpio_read(uint8_t cfg);
void od_gpio_park(uint8_t cfg);

#ifdef __cplusplus
}
#endif

#endif
