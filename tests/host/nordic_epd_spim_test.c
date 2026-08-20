#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "od_epd_spi.h"
#include "od_epd_spi_nrfx_test.h"

static unsigned s_bitbang_init_calls;
static unsigned s_bitbang_write_calls;
static unsigned s_bitbang_deinit_calls;

bool od_pin_decode(uint8_t cfg, uint8_t *port, uint8_t *pin)
{
	if (cfg == 0xffu) {
		return false;
	}
	*port = cfg >> 4;
	*pin = cfg & 0x0fu;
	return true;
}

bool od_board_epd_pin_reserved(uint8_t port, uint8_t pin)
{
	(void)port;
	(void)pin;
	return od_test_nrfx.reserved;
}

bool od_board_spim_pin_ok(uint8_t sck_port, uint8_t sck_pin,
			  uint8_t mosi_port, uint8_t mosi_pin)
{
	(void)sck_port;
	(void)sck_pin;
	(void)mosi_port;
	(void)mosi_pin;
	return od_test_nrfx.reachable;
}

#if !defined(OD_EPD_SPI_REQUIRE_SPIM)
bool od_epd_bitbang_init(uint8_t mosi_port, uint8_t mosi_pin,
			 uint8_t sck_port, uint8_t sck_pin)
{
	(void)mosi_port;
	(void)mosi_pin;
	(void)sck_port;
	(void)sck_pin;
	s_bitbang_init_calls++;
	return true;
}

bool od_epd_bitbang_write(const uint8_t *src, size_t len)
{
	(void)src;
	(void)len;
	s_bitbang_write_calls++;
	return true;
}

void od_epd_bitbang_deinit(void) { s_bitbang_deinit_calls++; }
uint32_t od_epd_bitbang_hz(void) { return 123456u; }
#endif

void _od_log(int level, const char *format, ...)
{
	(void)level;
	(void)format;
}

static void reset_fixture(void)
{
	od_test_nrfx_reset();
	s_bitbang_init_calls = 0u;
	s_bitbang_write_calls = 0u;
	s_bitbang_deinit_calls = 0u;
}

static uint8_t expected_instance(void)
{
#if defined(CONFIG_OD_PLATFORM_NRF52840)
	return 2u;
#elif defined(CONFIG_SOC_NRF54L15)
	return 0u;
#else
	return 23u;
#endif
}

static void test_bulk_spim(void)
{
	uint8_t source[600];

	for (size_t i = 0; i < sizeof(source); ++i) {
		source[i] = (uint8_t)i;
	}
	reset_fixture();
	assert(od_epd_spi_init(0x12u, 0x11u));
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_SPIM);
	assert(od_epd_spi_hz() == 8000000u);
	assert(!od_epd_spi_fault_reset());
	assert(od_test_nrfx.selected_instance == expected_instance());
	assert(od_test_nrfx.config.frequency == 8000000u);
	assert(od_test_nrfx.config.mode == NRF_SPIM_MODE_0);
	assert(od_test_nrfx.config.bit_order == NRF_SPIM_BIT_ORDER_MSB_FIRST);
	assert(od_test_nrfx.config.miso_pin == NRF_SPIM_PIN_NOT_CONNECTED);
	assert(od_test_nrfx.config.ss_pin == NRF_SPIM_PIN_NOT_CONNECTED);
	assert(od_test_nrfx.config.dcx_pin == 0xabcdef01u);
	assert(od_test_nrfx.config.rx_delay == 7u);
	assert(od_test_nrfx.config.ss_duration == 6u);
	assert(od_test_nrfx.irq_connect_calls == 1u);
	assert(od_epd_spi_init(0x12u, 0x11u));
	assert(od_test_nrfx.init_calls == 1u);

	assert(od_epd_spi_write(source, sizeof(source)));
	assert(od_test_nrfx.xfer_calls == 3u);
	assert(od_test_nrfx.chunk_lengths[0] == 256u);
	assert(od_test_nrfx.chunk_lengths[1] == 256u);
	assert(od_test_nrfx.chunk_lengths[2] == 88u);
	assert(od_test_nrfx.transmitted_len == sizeof(source));
	assert(memcmp(od_test_nrfx.transmitted, source, sizeof(source)) == 0);
	assert(od_test_nrfx.last_timeout_ms == 21u);
	od_epd_spi_deinit();
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_NONE);
	assert(od_test_nrfx.abort_calls == 1u);
	assert(od_test_nrfx.uninit_calls == 1u);
	assert(od_test_nrfx.pins_disconnect_calls == 1u);
#if NRF_GPIO_HAS_CLOCKPIN
	assert(od_test_nrfx.clock_clear_calls == 2u);
#else
	assert(od_test_nrfx.clock_clear_calls == 0u);
#endif
}

static void test_already_recovery(void)
{
	reset_fixture();
	od_test_nrfx.init_results[0] = -EALREADY;
	od_test_nrfx.init_results[1] = 0;
	od_test_nrfx.init_result_count = 2u;
	od_test_nrfx.stale_sck_pin = 33u;
	od_test_nrfx.stale_mosi_pin = 34u;
	assert(od_epd_spi_init(0x12u, 0x11u));
	assert(od_test_nrfx.init_calls == 2u);
	assert(od_test_nrfx.uninit_calls == 1u);
	assert(!od_epd_spi_faulted());
	od_epd_spi_deinit();
}

static void test_start_fault(void)
{
	const uint8_t byte = 0x5au;

	reset_fixture();
	assert(od_epd_spi_init(0x12u, 0x11u));
	od_test_nrfx.xfer_result = -EIO;
	assert(!od_epd_spi_write(&byte, 1u));
	assert(od_epd_spi_faulted());
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_SPIM);
	assert(s_bitbang_init_calls == 0u);
	assert(!od_epd_spi_write(&byte, 1u));
	od_epd_spi_deinit();
	assert(od_epd_spi_fault_reset());
	assert(!od_epd_spi_faulted());
}

static void test_init_fault(void)
{
	reset_fixture();
	od_test_nrfx.init_results[0] = -EIO;
	od_test_nrfx.init_result_count = 1u;
	assert(!od_epd_spi_init(0x12u, 0x11u));
	assert(od_epd_spi_faulted());
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_NONE);
	assert(s_bitbang_init_calls == 0u);
	assert(od_epd_spi_fault_reset());
}

static void test_timeout_and_late_callback(void)
{
	const uint8_t byte = 0xa5u;

	reset_fixture();
	assert(od_epd_spi_init(0x12u, 0x11u));
	od_test_nrfx.auto_done = false;
	assert(!od_epd_spi_write(&byte, 1u));
	assert(od_epd_spi_faulted());
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_NONE);
	assert(od_test_nrfx.last_timeout_ms == 21u);
	assert(od_test_nrfx.abort_calls == 1u);
	assert(od_test_nrfx.uninit_calls == 1u);
	od_test_nrfx_deliver_done();
	assert(od_epd_spi_fault_reset());
	od_test_nrfx.auto_done = true;
	assert(od_epd_spi_init(0x12u, 0x11u));
	assert(od_epd_spi_write(&byte, 1u));
	od_epd_spi_deinit();
}

static void test_selection_and_validation(void)
{
	reset_fixture();
	assert(!od_epd_spi_init(0x11u, 0x11u));
	assert(!od_epd_spi_faulted());
	od_test_nrfx.reserved = true;
	assert(!od_epd_spi_init(0x12u, 0x11u));
	assert(!od_epd_spi_faulted());
	od_test_nrfx.reserved = false;
	od_test_nrfx.reachable = false;
#if defined(OD_EPD_SPI_REQUIRE_SPIM)
	assert(!od_epd_spi_init(0x12u, 0x11u));
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_NONE);
#else
	assert(od_epd_spi_init(0x12u, 0x11u));
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_BITBANG);
	assert(od_epd_spi_hz() == 123456u);
	assert(s_bitbang_init_calls == 1u);
	assert(od_epd_spi_write((const uint8_t *)"x", 1u));
	assert(s_bitbang_write_calls == 1u);
	od_epd_spi_deinit();
	assert(s_bitbang_deinit_calls == 1u);
#endif
	assert(od_test_nrfx.init_calls == 0u);
	assert(!od_epd_spi_faulted());
}

int main(void)
{
	assert(od_epd_spi_backend() == OD_EPD_SPI_BACKEND_NONE);
	assert(od_epd_spi_fault_reset());
	test_bulk_spim();
	test_already_recovery();
	test_init_fault();
	test_start_fault();
	test_timeout_and_late_callback();
	test_selection_and_validation();
	return 0;
}
