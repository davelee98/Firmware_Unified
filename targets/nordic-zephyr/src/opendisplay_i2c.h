#ifndef OPENDISPLAY_I2C_H
#define OPENDISPLAY_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal bit-banged I2C master over the nRF54 GPIO helpers. Sensors run on a
 * data_bus (0x24) whose pins come from the runtime config, so the Zephyr I2C
 * peripheral (bound to fixed pads) cannot be used. Open-drain lines: a line is
 * driven low by configuring the pin as an output-low and released by
 * configuring it as an input with an (optional) internal pull-up so the bus
 * pull-up floats it high. Every wait (clock stretching) is bounded, so a stuck
 * line degrades to a failed transfer instead of hanging the main loop.
 */
struct od_i2c_bus {
	uint8_t scl_cfg;        /* nRF54 pin byte (port<<4)|pin, SCL */
	uint8_t sda_cfg;        /* nRF54 pin byte (port<<4)|pin, SDA */
	uint32_t half_period_us;
	bool scl_pullup;
	bool sda_pullup;
	bool ready;
};

/* Configure the bus from a data_bus definition. scl=pin_1, sda=pin_2.
 * speed_hz==0 defaults to 100 kHz. Returns false if either pin is invalid. */
bool od_i2c_init(struct od_i2c_bus *bus, uint8_t scl_cfg, uint8_t sda_cfg,
		 uint32_t speed_hz, bool scl_pullup, bool sda_pullup);

/* Write len bytes to addr7. If stop is false a repeated-start is expected next
 * (bus left with SCL low, no STOP). Returns true on full ACKed transfer. */
bool od_i2c_write(struct od_i2c_bus *bus, uint8_t addr7, const uint8_t *data,
		  size_t len, bool stop);

/* Read len bytes from addr7 (issues START + addr|R, master ACKs all but the
 * last byte, then STOP). Returns true on success. */
bool od_i2c_read(struct od_i2c_bus *bus, uint8_t addr7, uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
