#include "opendisplay_i2c.h"
#include "nrf54_gpio.h"

#include <zephyr/kernel.h>

/* Bounded clock-stretch timeout: never spin forever on a stuck SCL. */
#define OD_I2C_STRETCH_TIMEOUT_US 1000u

static inline void od_delay(const struct od_i2c_bus *bus)
{
	k_busy_wait(bus->half_period_us);
}

static inline void scl_low(const struct od_i2c_bus *bus)
{
	nrf54_gpio_configure_output(bus->scl_cfg, false);
}

static inline void sda_low(const struct od_i2c_bus *bus)
{
	nrf54_gpio_configure_output(bus->sda_cfg, false);
}

static inline void sda_release(const struct od_i2c_bus *bus)
{
	nrf54_gpio_configure_input(bus->sda_cfg, bus->sda_pullup, false);
}

static inline int sda_read(const struct od_i2c_bus *bus)
{
	return nrf54_gpio_read(bus->sda_cfg);
}

/* Release SCL and wait (bounded) for it to actually go high (clock stretch). */
static bool scl_release(const struct od_i2c_bus *bus)
{
	uint32_t waited = 0;

	nrf54_gpio_configure_input(bus->scl_cfg, bus->scl_pullup, false);
	while (nrf54_gpio_read(bus->scl_cfg) == 0) {
		if (waited >= OD_I2C_STRETCH_TIMEOUT_US) {
			return false;
		}
		k_busy_wait(1);
		waited++;
	}
	return true;
}

static void od_i2c_start(const struct od_i2c_bus *bus)
{
	/* Both lines high, then SDA falls while SCL high. */
	sda_release(bus);
	(void)scl_release(bus);
	od_delay(bus);
	sda_low(bus);
	od_delay(bus);
	scl_low(bus);
}

static void od_i2c_stop(const struct od_i2c_bus *bus)
{
	scl_low(bus);
	sda_low(bus);
	od_delay(bus);
	(void)scl_release(bus);
	od_delay(bus);
	sda_release(bus);
	od_delay(bus);
}

/* Clock out one bit (SDA already set). Returns false on SCL stretch timeout. */
static bool od_i2c_clock(const struct od_i2c_bus *bus)
{
	od_delay(bus);
	if (!scl_release(bus)) {
		return false;
	}
	od_delay(bus);
	scl_low(bus);
	return true;
}

/* Write a byte, return true if the slave ACKed. */
static bool od_i2c_write_byte(const struct od_i2c_bus *bus, uint8_t val)
{
	for (int i = 0; i < 8; i++) {
		scl_low(bus);
		if (val & 0x80u) {
			sda_release(bus);
		} else {
			sda_low(bus);
		}
		val = (uint8_t)(val << 1);
		if (!od_i2c_clock(bus)) {
			return false;
		}
	}
	/* ACK clock: release SDA, sample on the high edge. */
	scl_low(bus);
	sda_release(bus);
	od_delay(bus);
	if (!scl_release(bus)) {
		return false;
	}
	int ack = sda_read(bus);
	od_delay(bus);
	scl_low(bus);
	return ack == 0;
}

/* Read a byte, master drives ACK (ack=true) or NACK on the 9th clock. */
static bool od_i2c_read_byte(const struct od_i2c_bus *bus, uint8_t *out, bool ack)
{
	uint8_t val = 0;

	sda_release(bus);
	for (int i = 0; i < 8; i++) {
		scl_low(bus);
		od_delay(bus);
		if (!scl_release(bus)) {
			return false;
		}
		val = (uint8_t)(val << 1);
		if (sda_read(bus)) {
			val |= 1u;
		}
		od_delay(bus);
		scl_low(bus);
	}
	/* ACK/NACK bit. */
	scl_low(bus);
	if (ack) {
		sda_low(bus);
	} else {
		sda_release(bus);
	}
	if (!od_i2c_clock(bus)) {
		return false;
	}
	sda_release(bus);
	*out = val;
	return true;
}

bool od_i2c_init(struct od_i2c_bus *bus, uint8_t scl_cfg, uint8_t sda_cfg,
		 uint32_t speed_hz, bool scl_pullup, bool sda_pullup)
{
	uint8_t port;
	uint8_t pin;

	bus->ready = false;
	if (!nrf54_pin_decode(scl_cfg, &port, &pin) ||
	    !nrf54_pin_decode(sda_cfg, &port, &pin)) {
		return false;
	}
	if (speed_hz == 0u) {
		speed_hz = 100000u;
	}
	/* Half bit period in microseconds; clamp to >=1us (=> <=500 kHz). */
	uint32_t half = 500000u / speed_hz;
	if (half == 0u) {
		half = 1u;
	}
	bus->scl_cfg = scl_cfg;
	bus->sda_cfg = sda_cfg;
	bus->half_period_us = half;
	bus->scl_pullup = scl_pullup;
	bus->sda_pullup = sda_pullup;
	/* Idle both lines high. */
	sda_release(bus);
	(void)scl_release(bus);
	bus->ready = true;
	return true;
}

bool od_i2c_write(struct od_i2c_bus *bus, uint8_t addr7, const uint8_t *data,
		  size_t len, bool stop)
{
	if (!bus->ready) {
		return false;
	}
	od_i2c_start(bus);
	if (!od_i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 0u))) {
		od_i2c_stop(bus);
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (!od_i2c_write_byte(bus, data[i])) {
			od_i2c_stop(bus);
			return false;
		}
	}
	if (stop) {
		od_i2c_stop(bus);
	}
	return true;
}

bool od_i2c_read(struct od_i2c_bus *bus, uint8_t addr7, uint8_t *data, size_t len)
{
	if (!bus->ready || len == 0u) {
		return false;
	}
	od_i2c_start(bus);
	if (!od_i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 1u))) {
		od_i2c_stop(bus);
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		bool ack = (i + 1u) < len; /* ACK all but the last byte */
		if (!od_i2c_read_byte(bus, &data[i], ack)) {
			od_i2c_stop(bus);
			return false;
		}
	}
	od_i2c_stop(bus);
	return true;
}
