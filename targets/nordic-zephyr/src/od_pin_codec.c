#include "od_pin_codec.h"

#include <stddef.h>

#define OD_PIN_UNUSED 0xFFu

bool od_pin_decode_absolute(uint8_t cfg, uint8_t max_port,
			    uint8_t *port_out, uint8_t *pin_out)
{
	uint8_t port;

	if (cfg == OD_PIN_UNUSED || port_out == NULL || pin_out == NULL) {
		return false;
	}
	port = (uint8_t)(cfg >> 5);
	if (port > max_port) {
		return false;
	}
	*port_out = port;
	*pin_out = (uint8_t)(cfg & 0x1Fu);
	return true;
}

bool od_pin_decode_packed(uint8_t cfg, uint8_t max_port,
			  uint8_t *port_out, uint8_t *pin_out)
{
	uint8_t port;
	uint8_t pin;

	if (cfg == OD_PIN_UNUSED || port_out == NULL || pin_out == NULL) {
		return false;
	}
	if ((cfg & 0x80u) != 0u) {
		port = (uint8_t)((cfg >> 5) & 0x03u);
		pin = (uint8_t)(cfg & 0x1Fu);
	} else {
		port = (uint8_t)(cfg >> 4);
		pin = (uint8_t)(cfg & 0x0Fu);
	}
	if (port > max_port) {
		return false;
	}
	*port_out = port;
	*pin_out = pin;
	return true;
}
