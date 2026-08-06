#include "od_board.h"
#include "od_gpio.h"

#include <zephyr/kernel.h>

/* Bit-bang the 0xB9 deep power-down command to an external SPI NOR flash,
 * then park the bus. This behavior is common to every OpenDisplay Nordic
 * board; only pin decoding is platform-specific. */
void od_board_flash_powerdown(uint8_t mosi_cfg, uint8_t sck_cfg, uint8_t cs_cfg,
			      uint8_t miso_cfg, uint8_t wp_cfg, uint8_t hold_cfg)
{
	uint8_t cmd = 0xB9u;

	od_gpio_configure_output(mosi_cfg, false);
	od_gpio_configure_output(sck_cfg, false);
	od_gpio_configure_output(cs_cfg, false);
	if (miso_cfg != 0u && miso_cfg != 0xFFu) {
		od_gpio_configure_output(miso_cfg, false);
	}
	if (wp_cfg != 0u && wp_cfg != 0xFFu) {
		od_gpio_configure_output(wp_cfg, true);
	}
	if (hold_cfg != 0u && hold_cfg != 0xFFu) {
		od_gpio_configure_output(hold_cfg, true);
	}

	od_gpio_write(cs_cfg, false);
	for (uint8_t bit = 0; bit < 8u; bit++) {
		od_gpio_write(mosi_cfg, (cmd & 0x80u) != 0u);
		cmd = (uint8_t)(cmd << 1);
		k_busy_wait(1);
		od_gpio_write(sck_cfg, true);
		k_busy_wait(1);
		od_gpio_write(sck_cfg, false);
	}
	od_gpio_write(cs_cfg, true);
	k_busy_wait(30);

	od_gpio_write(mosi_cfg, false);
	od_gpio_write(sck_cfg, false);
	od_gpio_write(cs_cfg, true);
	if (miso_cfg != 0u && miso_cfg != 0xFFu) {
		od_gpio_write(miso_cfg, false);
	}
	if (wp_cfg != 0u && wp_cfg != 0xFFu) {
		od_gpio_write(wp_cfg, true);
	}
	if (hold_cfg != 0u && hold_cfg != 0xFFu) {
		od_gpio_write(hold_cfg, true);
	}
}
