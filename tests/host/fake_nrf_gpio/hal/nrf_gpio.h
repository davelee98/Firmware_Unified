#ifndef TEST_FAKE_NRF_GPIO_H
#define TEST_FAKE_NRF_GPIO_H

#include <stddef.h>
#include <stdint.h>

#define NRF_GPIO_HAS_RETENTION_SETCLEAR 1
#define NRF_GPIO_PIN_MAP(port, pin) ((((uint32_t)(port)) << 5) | (uint32_t)(pin))

typedef struct {
	uint8_t index;
} NRF_GPIO_Type;

struct od_test_gpio_event {
	char operation;
	uint8_t port;
	uint32_t value;
};

extern NRF_GPIO_Type od_test_gpio_ports[3];
extern struct od_test_gpio_event od_test_gpio_events[128];
extern size_t od_test_gpio_event_count;
extern unsigned od_test_gpio_decode_count;

static inline void od_test_gpio_record(char operation, NRF_GPIO_Type *port,
				       uint32_t value)
{
	od_test_gpio_events[od_test_gpio_event_count++] =
		(struct od_test_gpio_event){operation, port->index, value};
}

static inline NRF_GPIO_Type *nrf_gpio_pin_port_decode(uint32_t *pin)
{
	uint32_t port = *pin >> 5;

	od_test_gpio_decode_count++;
	*pin &= 31u;
	return port < 3u ? &od_test_gpio_ports[port] : NULL;
}

static inline void nrf_gpio_port_out_clear(NRF_GPIO_Type *port, uint32_t mask)
{
	od_test_gpio_record('C', port, mask);
}

static inline void nrf_gpio_port_out_set(NRF_GPIO_Type *port, uint32_t mask)
{
	od_test_gpio_record('S', port, mask);
}

static inline void nrf_gpio_port_pin_output_set(NRF_GPIO_Type *port, uint32_t pin)
{
	od_test_gpio_record('O', port, pin);
}

static inline void nrf_gpio_port_retain_disable(NRF_GPIO_Type *port, uint32_t mask)
{
	od_test_gpio_record('D', port, mask);
}

static inline void nrf_gpio_port_retain_enable(NRF_GPIO_Type *port, uint32_t mask)
{
	od_test_gpio_record('E', port, mask);
}

#endif
