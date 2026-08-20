#include "od_epd_spi.h"

#include "od_board.h"
#include "od_epd_sizes.h"
#include "od_gpio.h"
#include "od_log.h"

#if !defined(OD_EPD_SPI_REQUIRE_SPIM)
#include "od_epd_spi_bitbang.h"
#endif

#include <errno.h>
#include <string.h>

#if defined(OD_EPD_SPI_HOST_TEST)
#include "od_epd_spi_nrfx_test.h"
#else
#include <hal/nrf_spim.h>
#include <haly/nrfy_gpio.h>
#include <haly/nrfy_spim.h>
#include <nrfx_spim.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#endif

#define OD_EPD_SPI_HZ 8000000u

#if defined(CONFIG_OD_PLATFORM_NRF52840)
#define OD_EPD_SPIM_REG  NRF_SPIM2
#define OD_EPD_SPIM_NODE DT_NODELABEL(spi2)
#define OD_EPD_SPIM_NAME "SPIM2"
#elif defined(CONFIG_SOC_NRF54L15)
#define OD_EPD_SPIM_REG  NRF_SPIM00
#define OD_EPD_SPIM_NODE DT_NODELABEL(spi00)
#define OD_EPD_SPIM_NAME "SPIM00"
#elif defined(CONFIG_SOC_NRF54LM20A)
#define OD_EPD_SPIM_REG  NRF_SPIM23
#define OD_EPD_SPIM_NODE DT_NODELABEL(spi23)
#define OD_EPD_SPIM_NAME "SPIM23"
#else
#error "No panel SPIM instance selected for this Nordic board"
#endif

BUILD_ASSERT(OD_EPD_SPI_BOUNCE == OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE,
	     "panel DMA and decompression chunks must remain coupled");
BUILD_ASSERT(DT_NODE_EXISTS(OD_EPD_SPIM_NODE), "selected panel SPIM node is absent");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(OD_EPD_SPIM_NODE, okay),
	     "Zephyr SPI and the panel nrfx backend cannot own the same SPIM");

static nrfx_spim_t s_spim = NRFX_SPIM_INSTANCE(OD_EPD_SPIM_REG);
K_SEM_DEFINE(s_done, 0, 1);

static uint8_t s_dma[OD_EPD_SPI_BOUNCE] __aligned(4);
static od_epd_spi_backend_t s_backend;
static uint8_t s_mosi_cfg;
static uint8_t s_sck_cfg;
static uint32_t s_mosi_pin = NRF_SPIM_PIN_NOT_CONNECTED;
static uint32_t s_sck_pin = NRF_SPIM_PIN_NOT_CONNECTED;
static bool s_faulted;
static volatile bool s_wait_active;
static bool s_irq_connected;

#if !defined(OD_EPD_SPI_REQUIRE_SPIM)
static bool s_warned_pair_valid;
static uint8_t s_warned_mosi_port;
static uint8_t s_warned_mosi_pin;
static uint8_t s_warned_sck_port;
static uint8_t s_warned_sck_pin;
#endif

static void invalidate_saved_pins(void)
{
	s_mosi_cfg = UINT8_MAX;
	s_sck_cfg = UINT8_MAX;
	s_mosi_pin = NRF_SPIM_PIN_NOT_CONNECTED;
	s_sck_pin = NRF_SPIM_PIN_NOT_CONNECTED;
}

static void latch_fault(const char *operation, int status)
{
	if (!s_faulted) {
		s_faulted = true;
		od_log_error("panel SPI: %s %s failed: %d", OD_EPD_SPIM_NAME, operation, status);
	}
}

static void event_handler(nrfx_spim_event_t const *event, void *context)
{
	ARG_UNUSED(context);
	if (event->type == NRFX_SPIM_EVENT_DONE && s_wait_active) {
		s_wait_active = false;
		k_sem_give(&s_done);
	}
}

static void spim_isr(const void *arg)
{
	ARG_UNUSED(arg);
	nrfx_spim_irq_handler(&s_spim);
}

static void connect_irq_once(void)
{
	if (s_irq_connected) {
		return;
	}
	IRQ_CONNECT(DT_IRQN(OD_EPD_SPIM_NODE), DT_IRQ(OD_EPD_SPIM_NODE, priority),
		    spim_isr, NULL, 0);
	s_irq_connected = true;
}

static void clear_clockpin(uint32_t pin, bool needed)
{
#if NRF_GPIO_HAS_CLOCKPIN
	if (needed && pin != NRF_SPIM_PIN_NOT_CONNECTED) {
		nrfy_gpio_pin_clock_set(pin, false);
	}
#else
	ARG_UNUSED(pin);
	ARG_UNUSED(needed);
#endif
}

static void release_initialized_spim(uint32_t sck_pin, uint32_t mosi_pin)
{
	NRFX_IRQ_DISABLE(NRFX_IRQ_NUMBER_GET(OD_EPD_SPIM_REG));
	nrfx_spim_abort(&s_spim);
	nrfx_spim_uninit(&s_spim);
	nrf_spim_event_clear(OD_EPD_SPIM_REG, NRF_SPIM_EVENT_END);
	NRFX_IRQ_PENDING_CLEAR(NRFX_IRQ_NUMBER_GET(OD_EPD_SPIM_REG));
	nrf_spim_disable(OD_EPD_SPIM_REG);
	nrf_spim_pins_set(OD_EPD_SPIM_REG, NRF_SPIM_PIN_NOT_CONNECTED,
			  NRF_SPIM_PIN_NOT_CONNECTED, NRF_SPIM_PIN_NOT_CONNECTED);
#if defined(NRF_SPIM_CLOCKPIN_SCK_NEEDED)
	clear_clockpin(sck_pin, true);
#else
	clear_clockpin(sck_pin, false);
#endif
#if defined(NRF_SPIM_CLOCKPIN_MOSI_NEEDED)
	clear_clockpin(mosi_pin, true);
#else
	clear_clockpin(mosi_pin, false);
#endif
	k_sem_reset(&s_done);
}

static void recover_already_initialized(void)
{
	nrfy_spim_pins_t pins;

	nrfy_spim_pins_get(OD_EPD_SPIM_REG, &pins);
	od_log_error("panel SPI: %s was initialized outside backend state; recovering",
		     OD_EPD_SPIM_NAME);
	release_initialized_spim(pins.sck_pin, pins.mosi_pin);
}

static bool pins_valid(uint8_t mosi_cfg, uint8_t sck_cfg,
		       uint8_t *mosi_port, uint8_t *mosi_pin,
		       uint8_t *sck_port, uint8_t *sck_pin)
{
	if (!od_pin_decode(mosi_cfg, mosi_port, mosi_pin)
	    || !od_pin_decode(sck_cfg, sck_port, sck_pin)) {
		od_log_error("panel SPI: invalid runtime pins MOSI=0x%02X SCK=0x%02X",
			     mosi_cfg, sck_cfg);
		return false;
	}
	if (*mosi_port == *sck_port && *mosi_pin == *sck_pin) {
		od_log_error("panel SPI: MOSI and SCK both resolve to P%u.%02u",
			     *mosi_port, *mosi_pin);
		return false;
	}
	if (od_board_epd_pin_reserved(*mosi_port, *mosi_pin)
	    || od_board_epd_pin_reserved(*sck_port, *sck_pin)) {
		od_log_error("panel SPI: reserved runtime pins MOSI=P%u.%02u SCK=P%u.%02u",
			     *mosi_port, *mosi_pin, *sck_port, *sck_pin);
		return false;
	}
	return true;
}

bool od_epd_spi_init(uint8_t mosi_cfg, uint8_t sck_cfg)
{
	uint8_t mosi_port;
	uint8_t mosi_pin;
	uint8_t sck_port;
	uint8_t sck_pin;
	int status;

	if (s_faulted) {
		return false;
	}
	if (s_backend != OD_EPD_SPI_BACKEND_NONE) {
		if (s_mosi_cfg == mosi_cfg && s_sck_cfg == sck_cfg) {
			return true;
		}
		od_epd_spi_deinit();
	}
	if (!pins_valid(mosi_cfg, sck_cfg, &mosi_port, &mosi_pin, &sck_port, &sck_pin)) {
		invalidate_saved_pins();
		return false;
	}

	if (!od_board_spim_pin_ok(sck_port, sck_pin, mosi_port, mosi_pin)) {
#if defined(OD_EPD_SPI_REQUIRE_SPIM)
		od_log_error("panel SPI: %s cannot route SCK=P%u.%02u MOSI=P%u.%02u",
			     OD_EPD_SPIM_NAME, sck_port, sck_pin, mosi_port, mosi_pin);
		return false;
#else
		if (!od_epd_bitbang_init(mosi_port, mosi_pin, sck_port, sck_pin)) {
			od_log_error("panel SPI: direct GPIO acquire failed");
			invalidate_saved_pins();
			return false;
		}
		if (!s_warned_pair_valid || s_warned_mosi_port != mosi_port
		    || s_warned_mosi_pin != mosi_pin || s_warned_sck_port != sck_port
		    || s_warned_sck_pin != sck_pin) {
			uint32_t hz = od_epd_bitbang_hz();
			od_log_warn("panel SPI: %s cannot route SCK=P%u.%02u MOSI=P%u.%02u; "
				    "direct GPIO fallback rate=%u Hz%s",
				    OD_EPD_SPIM_NAME, sck_port, sck_pin, mosi_port, mosi_pin,
				    (unsigned)hz, hz == 0u ? " (hardware qualification pending)" : "");
			s_warned_pair_valid = true;
			s_warned_mosi_port = mosi_port;
			s_warned_mosi_pin = mosi_pin;
			s_warned_sck_port = sck_port;
			s_warned_sck_pin = sck_pin;
		}
		s_mosi_cfg = mosi_cfg;
		s_sck_cfg = sck_cfg;
		s_backend = OD_EPD_SPI_BACKEND_BITBANG;
		return true;
#endif
	}

	s_sck_pin = NRF_GPIO_PIN_MAP(sck_port, sck_pin);
	s_mosi_pin = NRF_GPIO_PIN_MAP(mosi_port, mosi_pin);
	nrfx_spim_config_t config = NRFX_SPIM_DEFAULT_CONFIG(
		s_sck_pin, s_mosi_pin, NRF_SPIM_PIN_NOT_CONNECTED, NRF_SPIM_PIN_NOT_CONNECTED);
	config.frequency = NRFX_MHZ_TO_HZ(8);
	config.mode = NRF_SPIM_MODE_0;
	config.bit_order = NRF_SPIM_BIT_ORDER_MSB_FIRST;
	config.irq_priority = DT_IRQ(OD_EPD_SPIM_NODE, priority);
	config.skip_gpio_cfg = false;
	config.skip_psel_cfg = false;

	connect_irq_once();
	status = nrfx_spim_init(&s_spim, &config, event_handler, NULL);
	if (status == -EALREADY) {
		recover_already_initialized();
		status = nrfx_spim_init(&s_spim, &config, event_handler, NULL);
	}
	if (status != 0) {
		latch_fault("init", status);
		invalidate_saved_pins();
		return false;
	}

	s_mosi_cfg = mosi_cfg;
	s_sck_cfg = sck_cfg;
	s_backend = OD_EPD_SPI_BACKEND_SPIM;
	od_log_debug("panel SPI: backend=SPIM instance=%s rate=%u Hz SCK=P%u.%02u MOSI=P%u.%02u",
		     OD_EPD_SPIM_NAME, OD_EPD_SPI_HZ, sck_port, sck_pin, mosi_port, mosi_pin);
	return true;
}

static bool spim_write_chunk(const uint8_t *src, size_t len)
{
	const uint32_t timeout_ms = DIV_ROUND_UP((uint32_t)len * 8u * 1000u,
						 OD_EPD_SPI_HZ) + 20u;
	nrfx_spim_xfer_desc_t transfer;
	int status;

	memcpy(s_dma, src, len);
	transfer = (nrfx_spim_xfer_desc_t)NRFX_SPIM_XFER_TX(s_dma, len);
	k_sem_reset(&s_done);
	s_wait_active = true;
	status = nrfx_spim_xfer(&s_spim, &transfer, 0);
	if (status != 0) {
		s_wait_active = false;
		latch_fault("transfer start", status);
		return false;
	}
	if (k_sem_take(&s_done, K_MSEC(timeout_ms)) != 0) {
		s_wait_active = false;
		latch_fault("completion timeout", -ETIMEDOUT);
		od_epd_spi_deinit();
		return false;
	}
	s_wait_active = false;
	return true;
}

bool od_epd_spi_write(const uint8_t *src, size_t len)
{
	if (s_faulted || s_backend == OD_EPD_SPI_BACKEND_NONE
	    || (src == NULL && len != 0u)) {
		return false;
	}
	if (len == 0u) {
		return true;
	}

#if !defined(OD_EPD_SPI_REQUIRE_SPIM)
	if (s_backend == OD_EPD_SPI_BACKEND_BITBANG) {
		return od_epd_bitbang_write(src, len);
	}
#endif
	for (size_t offset = 0; offset < len;) {
		size_t chunk = MIN(len - offset, sizeof(s_dma));
		if (!spim_write_chunk(src + offset, chunk)) {
			return false;
		}
		offset += chunk;
	}
	return true;
}

void od_epd_spi_deinit(void)
{
	switch (s_backend) {
	case OD_EPD_SPI_BACKEND_NONE:
		return;
#if !defined(OD_EPD_SPI_REQUIRE_SPIM)
	case OD_EPD_SPI_BACKEND_BITBANG:
		od_epd_bitbang_deinit();
		break;
#endif
	case OD_EPD_SPI_BACKEND_SPIM:
		s_wait_active = false;
		release_initialized_spim(s_sck_pin, s_mosi_pin);
		break;
	default:
		return;
	}
	s_backend = OD_EPD_SPI_BACKEND_NONE;
	invalidate_saved_pins();
}

bool od_epd_spi_faulted(void)
{
	return s_faulted;
}

bool od_epd_spi_fault_reset(void)
{
	if (s_backend != OD_EPD_SPI_BACKEND_NONE) {
		return false;
	}
	s_wait_active = false;
	nrf_spim_event_clear(OD_EPD_SPIM_REG, NRF_SPIM_EVENT_END);
	NRFX_IRQ_PENDING_CLEAR(NRFX_IRQ_NUMBER_GET(OD_EPD_SPIM_REG));
	k_sem_reset(&s_done);
	s_faulted = false;
	return true;
}

uint32_t od_epd_spi_hz(void)
{
	if (s_backend == OD_EPD_SPI_BACKEND_SPIM) {
		return OD_EPD_SPI_HZ;
	}
#if !defined(OD_EPD_SPI_REQUIRE_SPIM)
	if (s_backend == OD_EPD_SPI_BACKEND_BITBANG) {
		return od_epd_bitbang_hz();
	}
#endif
	return 0u;
}

od_epd_spi_backend_t od_epd_spi_backend(void)
{
	return s_backend;
}
