#include "od_epd_spi_bitbang.h"

#include <hal/nrf_gpio.h>

struct od_epd_direct_pin {
	NRF_GPIO_Type *port;
	uint32_t mask;
	uint8_t pin;
};

static struct od_epd_direct_pin s_sck;
static struct od_epd_direct_pin s_mosi;
static bool s_ready;

static void retain_disable(void)
{
#if NRF_GPIO_HAS_RETENTION_SETCLEAR
	nrf_gpio_port_retain_disable(s_sck.port, s_sck.mask);
	nrf_gpio_port_retain_disable(s_mosi.port, s_mosi.mask);
#endif
}

static void retain_enable(void)
{
#if NRF_GPIO_HAS_RETENTION_SETCLEAR
	nrf_gpio_port_retain_enable(s_sck.port, s_sck.mask);
	nrf_gpio_port_retain_enable(s_mosi.port, s_mosi.mask);
#endif
}

static bool resolve_pin(uint8_t port, uint8_t pin, struct od_epd_direct_pin *out)
{
	uint32_t relative = NRF_GPIO_PIN_MAP(port, pin);
	NRF_GPIO_Type *regs = nrf_gpio_pin_port_decode(&relative);

	if (regs == NULL || relative >= 32u) {
		return false;
	}
	out->port = regs;
	out->mask = 1u << relative;
	out->pin = (uint8_t)relative;
	return true;
}

bool od_epd_bitbang_init(uint8_t mosi_port, uint8_t mosi_pin,
			 uint8_t sck_port, uint8_t sck_pin)
{
	struct od_epd_direct_pin next_sck;
	struct od_epd_direct_pin next_mosi;

	if (!resolve_pin(sck_port, sck_pin, &next_sck)
	    || !resolve_pin(mosi_port, mosi_pin, &next_mosi)) {
		return false;
	}
	s_sck = next_sck;
	s_mosi = next_mosi;
	retain_disable();
	nrf_gpio_port_out_clear(s_sck.port, s_sck.mask);
	nrf_gpio_port_out_clear(s_mosi.port, s_mosi.mask);
	nrf_gpio_port_pin_output_set(s_sck.port, s_sck.pin);
	nrf_gpio_port_pin_output_set(s_mosi.port, s_mosi.pin);
	retain_enable();
	s_ready = true;
	return true;
}

bool od_epd_bitbang_write(const uint8_t *src, size_t len)
{
	if (!s_ready || (src == NULL && len != 0u)) {
		return false;
	}
	if (len == 0u) {
		return true;
	}

	retain_disable();
	for (size_t i = 0; i < len; ++i) {
		uint8_t byte = src[i];
		for (unsigned bit = 0; bit < 8u; ++bit) {
			nrf_gpio_port_out_clear(s_sck.port, s_sck.mask);
			if ((byte & 0x80u) != 0u) {
				nrf_gpio_port_out_set(s_mosi.port, s_mosi.mask);
			} else {
				nrf_gpio_port_out_clear(s_mosi.port, s_mosi.mask);
			}
			nrf_gpio_port_out_set(s_sck.port, s_sck.mask);
			byte <<= 1;
		}
	}
	nrf_gpio_port_out_clear(s_sck.port, s_sck.mask);
	retain_enable();
	return true;
}

void od_epd_bitbang_deinit(void)
{
	if (!s_ready) {
		return;
	}
	retain_disable();
	nrf_gpio_port_out_clear(s_sck.port, s_sck.mask);
	nrf_gpio_port_out_clear(s_mosi.port, s_mosi.mask);
	retain_enable();
	s_ready = false;
	s_sck = (struct od_epd_direct_pin){0};
	s_mosi = (struct od_epd_direct_pin){0};
}

uint32_t od_epd_bitbang_hz(void)
{
	/* Populated after the per-board hardware timing gate. Zero is deliberately
	 * distinguishable from the qualified 8 MHz SPIM path. */
	return 0u;
}
