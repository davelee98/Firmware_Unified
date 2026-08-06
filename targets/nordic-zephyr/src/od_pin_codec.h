#ifndef OD_PIN_CODEC_H
#define OD_PIN_CODEC_H

#include <stdbool.h>
#include <stdint.h>

/* Pure wire-format decoders. They intentionally do not inspect devicetree or
 * GPIO readiness, which keeps the encoding contract host-testable. */
bool od_pin_decode_absolute(uint8_t cfg, uint8_t max_port,
			    uint8_t *port_out, uint8_t *pin_out);
bool od_pin_decode_packed(uint8_t cfg, uint8_t max_port,
			  uint8_t *port_out, uint8_t *pin_out);

#endif
