#ifndef OPENDISPLAY_SENSOR_COMMON_H
#define OPENDISPLAY_SENSOR_COMMON_H

#include "opendisplay_ble.h"
#include "opendisplay_i2c.h"
#include "od_runtime_types.h"

/*
 * Resolve a sensor's data_bus (0x24) instance into a bit-banged I2C bus.
 * bus_type 0x01 = I2C, pin_1 = SCL, pin_2 = SDA (matches the reference
 * initOrRestoreWireForBus). Internal pull-ups are always enabled on release so
 * the bus works whether or not external pull-ups are fitted. Returns false when
 * the bus is missing, not I2C, or has invalid pins.
 */
static inline bool od_sensor_bus_for(uint8_t bus_id, struct od_i2c_bus *out)
{
	const struct od_config *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return false;
	}
	/* 0xFF means this device was never assigned a bus. Refused, not resolved to bus 0:
	 * substituting one probes it on somebody else's pins, where an address collision
	 * returns a plausible-but-wrong reading rather than a failure (DIVERGENCE_MATRIX 13).
	 * opendisplay_touch.c has always refused; this is the same rule. */
	if (bus_id == 0xFFu) {
		return false;
	}
	/* Resolved by instance_number, not indexed: od_config appends records in arrival order,
	 * so data_buses[bus_id] names the intended bus only while they arrive in ascending order
	 * with no gaps (DIVERGENCE_MATRIX 14). */
	const struct DataBus *bus = od_config_data_bus(cfg, bus_id);

	if (bus == NULL) {
		return false;
	}
	if (bus->bus_type != 0x01u) {
		return false;
	}
	if (bus->pin_1 == 0xFFu || bus->pin_2 == 0xFFu) {
		return false;
	}
	uint32_t hz = bus->bus_speed_hz ? bus->bus_speed_hz : 100000u;

	return od_i2c_init(out, bus->pin_1, bus->pin_2, hz, true, true);
}

#endif
