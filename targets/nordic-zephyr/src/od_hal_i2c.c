/* od_hal_i2c for nordic-zephyr, over the bit-banged engine in opendisplay_i2c.c.
 *
 * There is no bus MANAGER here, unlike ESP32: nothing constrains this target to one live bus, so
 * each operation resolves its bus and builds a local struct od_i2c_bus. That is what makes "a
 * completed call retains no bus ownership" free rather than a promise.
 *
 * WRITE_READ USES ONE BUS OBJECT FOR BOTH PHASES, and that is load-bearing. od_i2c_init() idles
 * SCL and SDA; re-initialising between the write and the read would drive the lines high in the
 * middle of a transaction, which is a STOP in all but name, and the repeated START would be gone.
 * The device answers a STOP-then-START read with whatever an unaddressed read yields -- plausible
 * values, not an error.
 */

#include "od_hal_i2c.h"

#include "od_config.h"
#include "opendisplay_ble.h"
#include "opendisplay_i2c.h"

/* bus_id is taken literally, 0xFF included: refusing the unassigned sentinel is the consumer's
 * rule (DIVERGENCE_MATRIX 13), and a transport that applied it too would make a record genuinely
 * numbered 0xFF unreachable. */
static bool select_bus(uint8_t bus_id, struct od_i2c_bus *out)
{
	const struct od_config *cfg = opendisplay_get_global_config();
	const struct DataBus *bus;

	if (cfg == NULL) {
		return false;
	}
	bus = od_config_data_bus(cfg, bus_id);
	if (bus == NULL || bus->bus_type != 0x01u) {
		return false;
	}
	if (bus->pin_1 == 0xFFu || bus->pin_2 == 0xFFu) {
		return false;
	}
	/* Internal pull-ups on both lines, as every consumer of this engine has always asked for. */
	return od_i2c_init(out, bus->pin_1, bus->pin_2, bus->bus_speed_hz, true, true);
}

static bool args_ok(uint8_t addr7, const void *p, uint16_t len)
{
	return addr7 <= 0x7Fu && p != NULL && len > 0u;
}

/* An address NACK is ENODEV and everything else is EIO, which is why the engine reports where it
 * failed rather than just that it did: collapsing the two makes a missing device indistinguishable
 * from a bus fault, and the other two targets keep them apart. */
static int from_engine(enum od_i2c_result r)
{
	switch (r) {
	case OD_I2C_RES_OK:        return OD_HAL_I2C_OK;
	case OD_I2C_RES_NACK_ADDR: return OD_HAL_I2C_ENODEV;
	default:                   return OD_HAL_I2C_EIO;
	}
}

int od_hal_i2c_probe(uint8_t bus_id, uint8_t addr7)
{
	struct od_i2c_bus bus;

	if (addr7 > 0x7Fu) {
		return OD_HAL_I2C_EINVAL;
	}
	if (!select_bus(bus_id, &bus)) {
		return OD_HAL_I2C_EINVAL;
	}
	/* Address-only: a zero-length write is not a portable presence check, which is why probe
	 * is its own operation. This engine can express it directly. */
	return from_engine(od_i2c_write_ex(&bus, addr7, NULL, 0u, true));
}

int od_hal_i2c_write(uint8_t bus_id, uint8_t addr7, const uint8_t *data, uint16_t len)
{
	struct od_i2c_bus bus;

	if (!args_ok(addr7, data, len)) {
		return OD_HAL_I2C_EINVAL;
	}
	if (!select_bus(bus_id, &bus)) {
		return OD_HAL_I2C_EINVAL;
	}
	return from_engine(od_i2c_write_ex(&bus, addr7, data, len, true));
}

int od_hal_i2c_read(uint8_t bus_id, uint8_t addr7, uint8_t *data, uint16_t len)
{
	struct od_i2c_bus bus;

	if (!args_ok(addr7, data, len)) {
		return OD_HAL_I2C_EINVAL;
	}
	if (!select_bus(bus_id, &bus)) {
		return OD_HAL_I2C_EINVAL;
	}
	return from_engine(od_i2c_read_ex(&bus, addr7, data, len));
}

int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7,
			  const uint8_t *tx, uint16_t tx_len,
			  uint8_t *rx, uint16_t rx_len)
{
	struct od_i2c_bus bus;

	if (!args_ok(addr7, tx, tx_len) || !args_ok(addr7, rx, rx_len)) {
		return OD_HAL_I2C_EINVAL;
	}
	if (!select_bus(bus_id, &bus)) {
		return OD_HAL_I2C_EINVAL;
	}
	/* stop=false leaves SCL low with no STOP, so od_i2c_read_ex()'s START is a REPEATED start.
	 * The SAME bus object carries both phases -- see the note at the top of this file. */
	enum od_i2c_result r = od_i2c_write_ex(&bus, addr7, tx, tx_len, false);

	if (r != OD_I2C_RES_OK) {
		return from_engine(r);
	}
	return from_engine(od_i2c_read_ex(&bus, addr7, rx, rx_len));
}
