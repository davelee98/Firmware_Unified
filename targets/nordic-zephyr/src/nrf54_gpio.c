#include "nrf54_gpio.h"

#include "od_log.h"

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
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio2), okay)
	case 2:
		return DEVICE_DT_GET(DT_NODELABEL(gpio2));
#endif
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
#if defined(OD_BOARD_XIAO_NRF52840)
	/*
	 * nRF52840 USES ABSOLUTE NORDIC PIN NUMBERS (port * 32 + pin), NOT the packed nibble
	 * form above. This is a wire contract with the host, not a preference: the Arduino
	 * nRF52 firmware this board is migrating from consumed the config's pin bytes as raw
	 * Nordic pin numbers, so every config already written for this hardware is in that
	 * encoding, and a real config off the bench decodes correctly only this way:
	 *
	 *   RST=15 -> P0.15      CS=44   -> P1.12 (D7,  XIAO CS)
	 *   BUSY=29 -> P0.29 (D3) DATA=47 -> P1.15 (D10, XIAO MOSI)
	 *   DC=31  -> P0.31      CLK=45  -> P1.13 (D8,  XIAO SCK)
	 *
	 * Under the nibble form those same bytes decode to port 2 (44 = 0x2C -> P2.12), a port
	 * the nRF52840 does not have. nrf54_pin_decode() then returned false for CS, MOSI and
	 * SCK, every bit-bang write became a no-op, and the panel stayed blank while BLE and
	 * PIPE upload worked perfectly -- which is exactly the reported symptom.
	 *
	 * The nRF54 boards keep the nibble form. They have a real P2, their deployed configs
	 * are written in that encoding, and changing them would break working hardware.
	 */
	port = (uint8_t)((cfg >> 5) & 0x07u);
	pin = (uint8_t)(cfg & 0x1Fu);
#else
	if ((cfg & 0x80u) != 0u) {
		port = (uint8_t)((cfg >> 5) & 0x03u);
		pin = (uint8_t)(cfg & 0x1Fu);
	} else {
		port = (uint8_t)((cfg >> 4) & 0x0Fu);
		pin = (uint8_t)(cfg & 0x0Fu);
	}
#endif
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
		od_log_debug("gpio: cfg=0x%02X does not decode; output IGNORED", cfg);
		return;
	}
	/*
	 * CHECKED, not (void)-ignored. Every one of these used to be discarded, which meant a pin
	 * Zephyr refused to configure -- claimed by a peripheral, absent from the board's DT --
	 * behaved exactly like a working one: the call returned, nothing was driven, and the pin
	 * dump still said ok=1 because THAT only proves the port exists and the encoding decodes.
	 * "Every API call succeeded and the panel is blank" is not a diagnosis; it is the absence
	 * of one.
	 */
	int err = gpio_pin_configure(gpio_dev(port), pin, flags);

	if (err != 0) {
		od_log_error("gpio: configure OUTPUT P%u.%02u (cfg=0x%02X) FAILED: %d",
			     port, pin, cfg, err);
	}
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
		od_log_debug("gpio: cfg=0x%02X does not decode; input IGNORED", cfg);
		return;
	}
	int err = gpio_pin_configure(gpio_dev(port), pin, flags);

	if (err != 0) {
		od_log_error("gpio: configure INPUT P%u.%02u (cfg=0x%02X) FAILED: %d",
			     port, pin, cfg, err);
	}
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
	int err = gpio_pin_set(gpio_dev(port), pin, level_high ? 1 : 0);

	/*
	 * Rate-limited to ONCE PER PIN. This is called per bit of bit-banged SPI -- 24 times per
	 * byte, ~1.15 million times for a 48 KB frame -- so an unconditional log here would be
	 * worse than the bug it reports. One line per failing pin is enough to identify it.
	 */
	if (err != 0) {
		static uint64_t reported;
		uint64_t bit = 1ULL << ((port * 32u) + pin);

		if ((reported & bit) == 0u) {
			reported |= bit;
			od_log_error("gpio: SET P%u.%02u (cfg=0x%02X) FAILED: %d "
				     "(further failures on this pin are suppressed)",
				     port, pin, cfg, err);
		}
	}
}

int nrf54_gpio_read(uint8_t cfg)
{
	uint8_t port;
	uint8_t pin;

	if (!nrf54_pin_decode(cfg, &port, &pin)) {
		return 0;
	}
	int val = gpio_pin_get(gpio_dev(port), pin);

	/*
	 * A NEGATIVE RETURN IS AN ERROR, NOT A LEVEL, and returning it raw was a trap: callers
	 * test it as a boolean, so an errno like -EIO reads as HIGH. For BUSY that means a broken
	 * read presents as "panel permanently busy" -- the opposite of the truth. Report once and
	 * return 0 so a failure cannot masquerade as a level.
	 */
	if (val < 0) {
		static uint64_t reported;
		uint64_t bit = 1ULL << ((port * 32u) + pin);

		if ((reported & bit) == 0u) {
			reported |= bit;
			od_log_error("gpio: GET P%u.%02u (cfg=0x%02X) FAILED: %d", port, pin, cfg,
				     val);
		}
		return 0;
	}
	return val;
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
