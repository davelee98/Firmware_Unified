#include "od_epd_spi_nrfx_test.h"

#include <string.h>

NRF_SPIM_Type od_test_spim2 = {2u};
NRF_SPIM_Type od_test_spim00 = {0u};
NRF_SPIM_Type od_test_spim23 = {23u};
struct od_test_nrfx_state od_test_nrfx;

static nrfx_spim_event_handler_t s_handler;
static void *s_context;

void od_test_nrfx_reset(void)
{
	memset(&od_test_nrfx, 0, sizeof(od_test_nrfx));
	od_test_nrfx.auto_done = true;
	od_test_nrfx.reachable = true;
	s_handler = NULL;
	s_context = NULL;
}

void od_test_nrfx_deliver_done(void)
{
	const nrfx_spim_event_t event = {.type = NRFX_SPIM_EVENT_DONE};

	if (s_handler != NULL) {
		s_handler(&event, s_context);
	}
}

void od_test_irq_connect(void) { od_test_nrfx.irq_connect_calls++; }
void od_test_irq_disable(void) { od_test_nrfx.irq_disable_calls++; }
void od_test_irq_pending_clear(void) { od_test_nrfx.irq_pending_clear_calls++; }

void k_sem_give(struct k_sem *sem)
{
	sem->count = 1u;
}

void k_sem_reset(struct k_sem *sem)
{
	sem->count = 0u;
	od_test_nrfx.sem_reset_calls++;
}

int k_sem_take(struct k_sem *sem, uint32_t timeout_ms)
{
	od_test_nrfx.last_timeout_ms = timeout_ms;
	if (sem->count == 0u) {
		return -1;
	}
	sem->count = 0u;
	return 0;
}

int nrfx_spim_init(nrfx_spim_t *instance, const nrfx_spim_config_t *config,
		   nrfx_spim_event_handler_t handler, void *context)
{
	int result = 0;
	od_test_nrfx.init_calls++;
	od_test_nrfx.selected_instance = instance->p_reg->instance;
	od_test_nrfx.config = *config;
	s_handler = handler;
	s_context = context;
	if (od_test_nrfx.init_result_index < od_test_nrfx.init_result_count) {
		result = od_test_nrfx.init_results[od_test_nrfx.init_result_index++];
	}
	return result;
}

int nrfx_spim_xfer(nrfx_spim_t *instance, const nrfx_spim_xfer_desc_t *transfer,
		   uint32_t flags)
{
	(void)instance;
	(void)flags;
	if (od_test_nrfx.xfer_result != 0) {
		return od_test_nrfx.xfer_result;
	}
	od_test_nrfx.chunk_lengths[od_test_nrfx.xfer_calls] = transfer->tx_length;
	od_test_nrfx.xfer_calls++;
	memcpy(od_test_nrfx.transmitted + od_test_nrfx.transmitted_len,
	       transfer->p_tx_buffer, transfer->tx_length);
	od_test_nrfx.transmitted_len += transfer->tx_length;
	if (od_test_nrfx.auto_done) {
		od_test_nrfx_deliver_done();
	}
	return 0;
}

void nrfx_spim_abort(nrfx_spim_t *instance)
{
	(void)instance;
	od_test_nrfx.abort_calls++;
}

void nrfx_spim_uninit(nrfx_spim_t *instance)
{
	(void)instance;
	od_test_nrfx.uninit_calls++;
}

void nrfx_spim_irq_handler(nrfx_spim_t *instance) { (void)instance; }

void nrfy_spim_pins_get(NRF_SPIM_Type *instance, nrfy_spim_pins_t *pins)
{
	(void)instance;
	pins->sck_pin = od_test_nrfx.stale_sck_pin;
	pins->mosi_pin = od_test_nrfx.stale_mosi_pin;
}

void nrf_spim_event_clear(NRF_SPIM_Type *instance, uint32_t event)
{
	(void)instance;
	(void)event;
	od_test_nrfx.event_clear_calls++;
}

void nrf_spim_disable(NRF_SPIM_Type *instance)
{
	(void)instance;
	od_test_nrfx.disable_calls++;
}

void nrf_spim_pins_set(NRF_SPIM_Type *instance, uint32_t sck, uint32_t mosi, uint32_t miso)
{
	(void)instance;
	if (sck == UINT32_MAX && mosi == UINT32_MAX && miso == UINT32_MAX) {
		od_test_nrfx.pins_disconnect_calls++;
	}
}

void nrfy_gpio_pin_clock_set(uint32_t pin, bool enabled)
{
	(void)pin;
	if (!enabled) {
		od_test_nrfx.clock_clear_calls++;
	}
}
