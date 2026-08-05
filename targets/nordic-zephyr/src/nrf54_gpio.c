#include "nrf54_gpio.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

static const struct device *gpio_dev(uint8_t port)
{
	switch (port) {
	case 0:
		return DEVICE_DT_GET(DT_NODELABEL(gpio0));
	case 1:
		return DEVICE_DT_GET(DT_NODELABEL(gpio1));
	case 2:
		return DEVICE_DT_GET(DT_NODELABEL(gpio2));
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio3), okay)
	case 3:
		return DEVICE_DT_GET(DT_NODELABEL(gpio3));
#endif
	default:
		return NULL;
	}
}

bool nrf54_pin_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out)
{
	uint8_t port;
	uint8_t pin;

	if (cfg == NRF54_GPIO_PIN_UNUSED) {
		return false;
	}
	/*
	 * Compact pin byte:
	 *   bit7=0: (port << 4) | pin   — pin 0..15 (legacy, L15-safe)
	 *   bit7=1: 0x80 | (port << 5) | pin — pin 0..31 (LM20 D1/D2/D3 etc.)
	 */
	if ((cfg & 0x80u) != 0u) {
		port = (uint8_t)((cfg >> 5) & 0x03u);
		pin = (uint8_t)(cfg & 0x1Fu);
	} else {
		port = (uint8_t)((cfg >> 4) & 0x0Fu);
		pin = (uint8_t)(cfg & 0x0Fu);
	}
	if (port > 3u || pin > 31u) {
		return false;
	}
	if (gpio_dev(port) == NULL || !device_is_ready(gpio_dev(port))) {
		return false;
	}
	*port_out = port;
	*pin_out = pin;
	return true;
}

void nrf54_gpio_configure_output(uint8_t cfg, bool initial_high)
{
	uint8_t port;
	uint8_t pin;
	gpio_flags_t flags = GPIO_OUTPUT | (initial_high ? GPIO_OUTPUT_INIT_HIGH
							 : GPIO_OUTPUT_INIT_LOW);

	if (!nrf54_pin_decode(cfg, &port, &pin)) {
		return;
	}
	(void)gpio_pin_configure(gpio_dev(port), pin, flags);
}

void nrf54_gpio_configure_input(uint8_t cfg, bool pull_up, bool pull_down)
{
	uint8_t port;
	uint8_t pin;
	gpio_flags_t flags = GPIO_INPUT;

	if (pull_up) {
		flags |= GPIO_PULL_UP;
	} else if (pull_down) {
		flags |= GPIO_PULL_DOWN;
	}
	if (!nrf54_pin_decode(cfg, &port, &pin)) {
		return;
	}
	(void)gpio_pin_configure(gpio_dev(port), pin, flags);
}

/* One slot per interrupt-enabled pin. Each slot owns its gpio_callback so the
 * Zephyr trampoline can recover the registered handler via CONTAINER_OF. */
#define NRF54_GPIO_IRQ_MAX 8

struct nrf54_gpio_irq_slot {
	struct gpio_callback cb;
	nrf54_gpio_irq_handler_t handler;
	bool used;
};

static struct nrf54_gpio_irq_slot s_irq_slots[NRF54_GPIO_IRQ_MAX];

static void nrf54_gpio_irq_trampoline(const struct device *dev, struct gpio_callback *cb,
				      uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pins);
	struct nrf54_gpio_irq_slot *slot =
		CONTAINER_OF(cb, struct nrf54_gpio_irq_slot, cb);

	if (slot->handler != NULL) {
		slot->handler();
	}
}

int nrf54_gpio_configure_interrupt(uint8_t cfg, nrf54_gpio_irq_handler_t handler)
{
	uint8_t port;
	uint8_t pin;
	const struct device *dev;
	struct nrf54_gpio_irq_slot *slot = NULL;
	int err;

	if (handler == NULL || !nrf54_pin_decode(cfg, &port, &pin)) {
		return -1;
	}
	dev = gpio_dev(port);
	for (unsigned i = 0; i < NRF54_GPIO_IRQ_MAX; i++) {
		if (!s_irq_slots[i].used) {
			slot = &s_irq_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		return -1;
	}
	slot->handler = handler;
	slot->used = true;
	gpio_init_callback(&slot->cb, nrf54_gpio_irq_trampoline, BIT(pin));
	err = gpio_add_callback(dev, &slot->cb);
	if (err != 0) {
		slot->used = false;
		return err;
	}
	err = gpio_pin_interrupt_configure(dev, pin, GPIO_INT_EDGE_BOTH);
	if (err != 0) {
		(void)gpio_remove_callback(dev, &slot->cb);
		slot->used = false;
	}
	return err;
}

void nrf54_gpio_write(uint8_t cfg, bool level_high)
{
	uint8_t port;
	uint8_t pin;

	if (!nrf54_pin_decode(cfg, &port, &pin)) {
		return;
	}
	(void)gpio_pin_set(gpio_dev(port), pin, level_high ? 1 : 0);
}

int nrf54_gpio_read(uint8_t cfg)
{
	uint8_t port;
	uint8_t pin;

	if (!nrf54_pin_decode(cfg, &port, &pin)) {
		return 0;
	}
	return gpio_pin_get(gpio_dev(port), pin);
}

void nrf54_gpio_park(uint8_t cfg)
{
	uint8_t port;
	uint8_t pin;

	if (!nrf54_pin_decode(cfg, &port, &pin)) {
		return;
	}
	(void)gpio_pin_configure(gpio_dev(port), pin, GPIO_DISCONNECTED);
}

ssize_t od_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	return hwinfo_get_device_id(buffer, length);
}
