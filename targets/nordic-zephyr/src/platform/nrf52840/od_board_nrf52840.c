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
