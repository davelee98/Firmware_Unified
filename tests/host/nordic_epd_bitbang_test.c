#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal/nrf_gpio.h"
#include "od_epd_spi_bitbang.h"

NRF_GPIO_Type od_test_gpio_ports[3] = {{0}, {1}, {2}};
struct od_test_gpio_event od_test_gpio_events[128];
size_t od_test_gpio_event_count;
unsigned od_test_gpio_decode_count;

static void clear_events(void)
{
	od_test_gpio_event_count = 0u;
}

static void expect_event(size_t index, char operation, uint8_t port, uint32_t value)
{
	assert(index < od_test_gpio_event_count);
	assert(od_test_gpio_events[index].operation == operation);
	assert(od_test_gpio_events[index].port == port);
	assert(od_test_gpio_events[index].value == value);
}

int main(void)
{
	const uint8_t byte = 0xa5u;
	const uint8_t bits[8] = {1u, 0u, 1u, 0u, 0u, 1u, 0u, 1u};
	const uint32_t sck_mask = 1u << 1;
	const uint32_t mosi_mask = 1u << 2;

	assert(!od_epd_bitbang_write(&byte, 1u));
	assert(!od_epd_bitbang_init(2u, 2u, 3u, 1u));
	assert(od_epd_bitbang_init(2u, 2u, 2u, 1u));
	assert(od_test_gpio_decode_count == 3u);
	assert(od_epd_bitbang_hz() == 0u);

	clear_events();
	assert(od_epd_bitbang_write(&byte, 1u));
	assert(od_test_gpio_decode_count == 3u);
	assert(od_test_gpio_event_count == 29u);
	expect_event(0u, 'D', 2u, sck_mask);
	expect_event(1u, 'D', 2u, mosi_mask);
	for (size_t bit = 0; bit < 8u; ++bit) {
		size_t base = 2u + bit * 3u;
		expect_event(base, 'C', 2u, sck_mask);
		expect_event(base + 1u, bits[bit] ? 'S' : 'C', 2u, mosi_mask);
		expect_event(base + 2u, 'S', 2u, sck_mask);
	}
	expect_event(26u, 'C', 2u, sck_mask);
	expect_event(27u, 'E', 2u, sck_mask);
	expect_event(28u, 'E', 2u, mosi_mask);

	clear_events();
	assert(od_epd_bitbang_write(NULL, 0u));
	assert(od_test_gpio_event_count == 0u);
	assert(!od_epd_bitbang_write(NULL, 1u));

	od_epd_bitbang_deinit();
	assert(!od_epd_bitbang_write(&byte, 1u));
	return 0;
}
