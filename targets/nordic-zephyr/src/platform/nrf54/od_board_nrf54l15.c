#include "od_board.h"
#include "od_gpio.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define NRF54L15_RFSW_PWR OD_GPIO_CFG(2, 3)
#define NRF54L15_RFSW_SEL OD_GPIO_CFG(2, 5)
#define NRF54L15_BS_PIN   OD_GPIO_CFG(2, 10)

BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_NRF54L15),
	     "nRF54L15 board support compiled for the wrong SoC");

const char *od_board_name(void)
{
	return "XIAO nRF54L15";
}

void od_board_early_init(void)
{
	od_gpio_configure_output(NRF54L15_RFSW_PWR, true);
	od_gpio_configure_output(NRF54L15_RFSW_SEL, false);
	od_gpio_configure_output(NRF54L15_BS_PIN, false);
	k_msleep(10);
}

void od_board_prepare_epd_rail(void)
{
	od_gpio_write(NRF54L15_BS_PIN, false);
}

bool od_board_epd_requires_cold_cycle(void)
{
	return false;
}
