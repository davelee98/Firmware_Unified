#ifndef OD_EPD_SPI_NRFX_TEST_H
#define OD_EPD_SPI_NRFX_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARG_UNUSED(value) ((void)(value))
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1u) / (d))
#define __aligned(value) __attribute__((aligned(value)))

#define DT_NODELABEL(name) 1
#define DT_NODE_EXISTS(node) 1
#define DT_NODE_HAS_STATUS(node, status) 0
#define DT_IRQN(node) 7
#define DT_IRQ(node, cell) 3

typedef struct {
	uint8_t instance;
} NRF_SPIM_Type;

extern NRF_SPIM_Type od_test_spim2;
extern NRF_SPIM_Type od_test_spim00;
extern NRF_SPIM_Type od_test_spim23;

#define NRF_SPIM2  (&od_test_spim2)
#define NRF_SPIM00 (&od_test_spim00)
#define NRF_SPIM23 (&od_test_spim23)

#define NRF_SPIM_PIN_NOT_CONNECTED UINT32_MAX
#define NRF_GPIO_PIN_MAP(port, pin) ((((uint32_t)(port)) << 5) | (uint32_t)(pin))
#define NRF_SPIM_MODE_0 0u
#define NRF_SPIM_BIT_ORDER_MSB_FIRST 0u
#define NRFX_MHZ_TO_HZ(mhz) ((uint32_t)(mhz) * 1000000u)

#if defined(CONFIG_SOC_NRF54L15) || defined(CONFIG_SOC_NRF54LM20A)
#define NRF_GPIO_HAS_CLOCKPIN 1
#define NRF_SPIM_CLOCKPIN_SCK_NEEDED 1
#define NRF_SPIM_CLOCKPIN_MOSI_NEEDED 1
#else
#define NRF_GPIO_HAS_CLOCKPIN 0
#endif

typedef struct {
	uint32_t sck_pin;
	uint32_t mosi_pin;
	uint32_t miso_pin;
	uint32_t ss_pin;
	bool ss_active_high;
	uint8_t irq_priority;
	uint8_t orc;
	uint32_t frequency;
	uint8_t mode;
	uint8_t bit_order;
	uint8_t miso_pull;
	uint32_t dcx_pin;
	uint8_t rx_delay;
	bool use_hw_ss;
	uint8_t ss_duration;
	bool skip_gpio_cfg;
	bool skip_psel_cfg;
} nrfx_spim_config_t;

#define NRFX_SPIM_DEFAULT_CONFIG(sck, mosi, miso, ss) \
	((nrfx_spim_config_t){ \
		.sck_pin = (sck), .mosi_pin = (mosi), .miso_pin = (miso), .ss_pin = (ss), \
		.irq_priority = 9u, .orc = 0xffu, .frequency = 1000000u, \
		.dcx_pin = 0xabcdef01u, .rx_delay = 7u, .use_hw_ss = false, .ss_duration = 6u })

typedef struct {
	NRF_SPIM_Type *p_reg;
} nrfx_spim_t;

#define NRFX_SPIM_INSTANCE(reg) { .p_reg = (reg) }

typedef struct {
	const uint8_t *p_tx_buffer;
	size_t tx_length;
} nrfx_spim_xfer_desc_t;

#define NRFX_SPIM_XFER_TX(buffer, length) \
	((nrfx_spim_xfer_desc_t){ .p_tx_buffer = (buffer), .tx_length = (length) })

typedef enum {
	NRFX_SPIM_EVENT_DONE = 1,
} nrfx_spim_evt_type_t;

typedef struct {
	nrfx_spim_evt_type_t type;
} nrfx_spim_event_t;

typedef void (*nrfx_spim_event_handler_t)(const nrfx_spim_event_t *event, void *context);

struct k_sem {
	unsigned count;
};

#define K_SEM_DEFINE(name, initial, limit) struct k_sem name = { (initial) }
#define K_MSEC(ms) (ms)

typedef struct {
	uint32_t sck_pin;
	uint32_t mosi_pin;
} nrfy_spim_pins_t;

struct od_test_nrfx_state {
	nrfx_spim_config_t config;
	int init_results[3];
	unsigned init_result_count;
	unsigned init_result_index;
	int xfer_result;
	bool auto_done;
	bool reserved;
	bool reachable;
	unsigned init_calls;
	unsigned xfer_calls;
	unsigned abort_calls;
	unsigned uninit_calls;
	unsigned irq_connect_calls;
	unsigned irq_disable_calls;
	unsigned irq_pending_clear_calls;
	unsigned event_clear_calls;
	unsigned disable_calls;
	unsigned pins_disconnect_calls;
	unsigned sem_reset_calls;
	unsigned clock_clear_calls;
	uint8_t selected_instance;
	uint32_t last_timeout_ms;
	uint32_t stale_sck_pin;
	uint32_t stale_mosi_pin;
	size_t chunk_lengths[8];
	uint8_t transmitted[1024];
	size_t transmitted_len;
};

extern struct od_test_nrfx_state od_test_nrfx;
void od_test_nrfx_reset(void);
void od_test_nrfx_deliver_done(void);
void od_test_irq_connect(void);

#define IRQ_CONNECT(irq, priority, isr, arg, flags) \
	do { (void)(isr); od_test_irq_connect(); } while (0)
#define NRFX_IRQ_NUMBER_GET(reg) 7
#define NRFX_IRQ_DISABLE(irq) od_test_irq_disable()
#define NRFX_IRQ_PENDING_CLEAR(irq) od_test_irq_pending_clear()

void od_test_irq_disable(void);
void od_test_irq_pending_clear(void);
void k_sem_give(struct k_sem *sem);
void k_sem_reset(struct k_sem *sem);
int k_sem_take(struct k_sem *sem, uint32_t timeout_ms);
int nrfx_spim_init(nrfx_spim_t *instance, const nrfx_spim_config_t *config,
		   nrfx_spim_event_handler_t handler, void *context);
int nrfx_spim_xfer(nrfx_spim_t *instance, const nrfx_spim_xfer_desc_t *transfer,
		   uint32_t flags);
void nrfx_spim_abort(nrfx_spim_t *instance);
void nrfx_spim_uninit(nrfx_spim_t *instance);
void nrfx_spim_irq_handler(nrfx_spim_t *instance);
void nrfy_spim_pins_get(NRF_SPIM_Type *instance, nrfy_spim_pins_t *pins);
void nrf_spim_event_clear(NRF_SPIM_Type *instance, uint32_t event);
void nrf_spim_disable(NRF_SPIM_Type *instance);
void nrf_spim_pins_set(NRF_SPIM_Type *instance, uint32_t sck, uint32_t mosi, uint32_t miso);
void nrfy_gpio_pin_clock_set(uint32_t pin, bool enabled);

#define NRF_SPIM_EVENT_END 1u

#endif
