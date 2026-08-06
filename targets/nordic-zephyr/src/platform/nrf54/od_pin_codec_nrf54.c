#include "od_gpio.h"
#include "od_pin_codec.h"

#include <zephyr/sys/util.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_OD_PLATFORM_NRF54),
	     "nRF54 pin codec compiled for the wrong platform");
BUILD_ASSERT(!IS_ENABLED(CONFIG_OD_PLATFORM_NRF52840),
	     "nRF52840 and nRF54 platform selections are mutually exclusive");

bool od_pin_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out)
{
	uint8_t port;
	uint8_t pin;

	if (!od_pin_decode_packed(cfg, 3u, &port, &pin) || !od_gpio_port_ready(port)) {
		return false;
	}

	*port_out = port;
	*pin_out = pin;
	return true;
}
