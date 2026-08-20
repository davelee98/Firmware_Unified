#include "od_board.h"
#include "od_gpio.h"

#include <zephyr/sys/util.h>

#define NRF52840_EPD_BS_PIN 13u /* absolute P0.13, inherited board contract */

BUILD_ASSERT(IS_ENABLED(CONFIG_OD_PLATFORM_NRF52840),
	     "nRF52840 board support compiled for the wrong platform");

const char *od_board_name(void)
{
	return "XIAO nRF52840";
}

void od_board_early_init(void)
{
	/* The donor Bluefruit firmware's xiaoinit() drives this line low before
	 * any display work. It selects the EPD boost path on deployed hardware. */
	od_gpio_configure_output(NRF52840_EPD_BS_PIN, false);
}

void od_board_prepare_epd_rail(void)
{
	od_gpio_configure_output(NRF52840_EPD_BS_PIN, false);
}

bool od_board_epd_requires_cold_cycle(void)
{
	/* Matches prepareEpdRailForBoot() in the donor firmware. */
	return true;
}

bool od_board_epd_pin_reserved(uint8_t port, uint8_t pin)
{
	/* P0.13 selects the EPD boost path. P0.20..25 belong to the
	 * board's enabled QSPI flash. */
	return port == 0u && (pin == 13u || (pin >= 20u && pin <= 25u));
}

bool od_board_spim_pin_ok(uint8_t sck_port, uint8_t sck_pin,
			  uint8_t mosi_port, uint8_t mosi_pin)
{
	(void)sck_pin;
	(void)mosi_pin;
	return sck_port <= 1u && mosi_port <= 1u;
}
