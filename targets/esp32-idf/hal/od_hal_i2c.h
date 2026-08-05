/* od_hal_i2c -- the I2C master bus and its transactions.
 *
 * SHAPE DEVIATES FROM docs/SHARED_API_DESIGN.md, deliberately. That sketch is
 * register-oriented:
 *
 *     int od_hal_i2c_read (uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
 *     int od_hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, uint16_t len);
 *
 * and it cannot express what these drivers actually do. The SHT40 measurement is
 * write-command, STOP, wait ~9 ms for the conversion, then a bare read -- there is no register
 * to name, and the delay has to sit between two separate transactions. The BQ27220 is the
 * opposite: write-selector then read with a REPEATED START and no STOP, which is a single
 * transaction. A register-shaped call collapses both into "write then read" and gets the bus
 * framing wrong for each in a different direction.
 *
 * So this exposes primitives and lets the drivers compose them. The doc's two calls are
 * expressible on top; the reverse is not. Flagged rather than silently diverged: the sketch was
 * written from reading, and this is what building it found.
 *
 * THE CORE DOES NOT CALL I2C. Sensors and the PMIC are target drivers, which is why this lives
 * in targets/ and is not on the list of things heading for shared/hal. It exists because IDF's
 * i2c_master driver has ONE bus handle per port and several drivers need it -- a bus manager,
 * not a portability layer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. Deliberately NOT Arduino's endTransmission() encoding (0 ok, 1..4 error) --
 * that inverted convention belongs at the Arduino boundary, and compat/Wire.h translates. */
#define OD_HAL_I2C_OK        0
#define OD_HAL_I2C_ERR      (-1)   /* bus fault, timeout, or a peer that NACKed data */
#define OD_HAL_I2C_ENODEV   (-2)   /* nobody acknowledged the address */
#define OD_HAL_I2C_EINVAL   (-3)   /* bad argument, or the bus is not up */

/* Brings the bus up on the given pins. Idempotent for the SAME pins -- returns true without
 * touching hardware. Different pins require od_hal_i2c_deinit() first; this does NOT silently
 * reconfigure, because the caller that switches buses (display_service.cpp) tracks which bus is
 * open and a silent switch would hide a mismatch between its bookkeeping and the hardware. */
bool od_hal_i2c_init(uint8_t sda, uint8_t scl, uint32_t hz);

/* Tears the bus down, releasing any cached device handle. Safe when the bus is already down. */
void od_hal_i2c_deinit(void);

bool od_hal_i2c_is_up(void);

/* IDF fixes the clock per DEVICE, not per bus, so this only records the value; it takes effect
 * the next time a device handle is attached. Arduino's setClock() could be called any time,
 * and callers were written against that, so the looseness is preserved rather than made an
 * error. */
void od_hal_i2c_set_clock(uint32_t hz);

/* Address-only presence check: START, address, STOP, no data phase.
 *
 * This is its own primitive and not "a write of length zero" because IDF REJECTS a zero-length
 * i2c_master_transmit() outright (i2c_master.c: "i2c transmit buffer or size invalid"), so
 * nothing reaches the bus -- no START, no address byte, no ACK to observe. A scan built on a
 * zero-length write therefore reports "absent" for every address whether or not hardware is
 * there. i2c_master_probe() is IDF's primitive for exactly this and takes the BUS handle, so a
 * scan needs no per-address device registration. */
int od_hal_i2c_probe(uint8_t addr);

int od_hal_i2c_write(uint8_t addr, const uint8_t *buf, uint16_t len);

int od_hal_i2c_read(uint8_t addr, uint8_t *buf, uint16_t len);

/* Write then read as ONE transaction, with a repeated START and no STOP between -- the
 * register-read idiom. Not the same as write() followed by read(): the BQ27220 does not accept
 * STOP + fresh START for a register read and returns whatever an unaddressed read yields, so
 * getting this wrong produces plausible garbage rather than an error. */
int od_hal_i2c_write_read(uint8_t addr, const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_len);

#ifdef __cplusplus
}
#endif
