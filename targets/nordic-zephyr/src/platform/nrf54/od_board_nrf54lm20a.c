#include "od_board.h"
#include "od_gpio.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define NRF54LM20_POWER_EN OD_GPIO_CFG(1, 12)
#define NRF54LM20_MIC_CLK  OD_GPIO_CFG(1, 13)
#define NRF54LM20_MIC_DIN  OD_GPIO_CFG(1, 14)
#define NRF54LM20_UART_RX  OD_GPIO_CFG(1, 10)
#define NRF54LM20_UART_TX  OD_GPIO_CFG(1, 11)

BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_NRF54LM20A),
	     "nRF54LM20A board support compiled for the wrong SoC");

const char *od_board_name(void)
{
	return "XIAO nRF54LM20A";
}

void od_board_early_init(void)
{
	od_gpio_configure_output(NRF54LM20_POWER_EN, true);
	od_gpio_park(NRF54LM20_MIC_CLK);
	od_gpio_park(NRF54LM20_MIC_DIN);
	od_gpio_park(NRF54LM20_UART_RX);
	od_gpio_park(NRF54LM20_UART_TX);
	k_msleep(50);
}

void od_board_prepare_epd_rail(void)
{
}

bool od_board_epd_requires_cold_cycle(void)
{
	return false;
}
