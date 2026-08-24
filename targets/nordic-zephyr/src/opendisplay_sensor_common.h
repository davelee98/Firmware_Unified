#ifndef OPENDISPLAY_SENSOR_COMMON_H
#define OPENDISPLAY_SENSOR_COMMON_H

#include "opendisplay_ble.h"
#include "od_runtime_types.h"

/*
 * od_sensor_bus_for() lived here and is gone. Every sensor now names a bus_id and the shared
 * od_hal_i2c operations resolve it, so nothing needs a caller-owned bus object.
 *
 * What that helper carried, and where it went: 0xFF means UNASSIGNED and each sensor refuses it
 * itself, because the transport takes its bus argument literally (DIVERGENCE_MATRIX 13); and the
 * bus is resolved by DataBus.instance_number rather than array position (DIVERGENCE_MATRIX 14).
 * Internal pull-ups on both lines are still requested, now by targets/nordic-zephyr/src/od_hal_i2c.c.
 */

#endif
