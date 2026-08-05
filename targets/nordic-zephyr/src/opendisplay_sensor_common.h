#ifndef OPENDISPLAY_SENSOR_COMMON_H
#define OPENDISPLAY_SENSOR_COMMON_H

#include "opendisplay_ble.h"
#include "opendisplay_i2c.h"
#include "opendisplay_structs.h"

/*
 * Resolve a sensor's data_bus (0x24) instance into a bit-banged I2C bus.
 * bus_type 0x01 = I2C, pin_1 = SCL, pin_2 = SDA (matches the reference
 * initOrRestoreWireForBus). Internal pull-ups are always enabled on release so
 * the bus works whether or not external pull-ups are fitted. Returns false when
 * the bus is missing, not I2C, or has invalid pins.
 */
static inline bool od_sensor_bus_for(uint8_t bus_id, struct od_i2c_bus *out)
{
	const struct GlobalConfig *cfg = opendisplay_get_global_config();

	if (cfg == NULL) {
		return false;
	}
	if (bus_id == 0xFFu) {
		bus_id = 0u;
	}
	if (bus_id >= cfg->data_bus_count || bus_id >= 4u) {
		return false;
	}
	const struct DataBus *bus = &cfg->data_buses[bus_id];

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
