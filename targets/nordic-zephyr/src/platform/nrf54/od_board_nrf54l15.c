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

bool od_board_epd_pin_reserved(uint8_t port, uint8_t pin)
{
	return port == 2u && (pin == 3u || pin == 5u || pin == 10u);
}

bool od_board_spim_pin_ok(uint8_t sck_port, uint8_t sck_pin,
			  uint8_t mosi_port, uint8_t mosi_pin)
{
	return (sck_port == 2u && sck_pin == 1u && mosi_port == 2u && mosi_pin == 2u)
	    || (sck_port == 2u && sck_pin == 6u && mosi_port == 2u && mosi_pin == 8u);
}
