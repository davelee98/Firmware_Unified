#include "od_gpio.h"
#include "od_pin_codec.h"

#include <zephyr/sys/util.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_OD_PLATFORM_NRF52840),
	     "nRF52840 pin codec compiled for the wrong platform");
BUILD_ASSERT(!IS_ENABLED(CONFIG_OD_PLATFORM_NRF54),
	     "nRF52840 and nRF54 platform selections are mutually exclusive");

#if IS_ENABLED(CONFIG_NFC_T2T_NRFXLIB)
/* P0.09 and P0.10 are this SoC's NFC antenna pair, and the reservation is STATIC: it holds
 * whenever the tag is built, not while it happens to be running.
 *
 * IT CANNOT BE OTHERWISE. Which function owns these pads is the UICR NFCPINS latch, read at
 * reset -- no runtime state changes it, so a refusal keyed on whether NFC is currently emulating
 * would describe a decision the hardware already made. The pads are NFC-owned from reset whether
 * or not any nfc_config (0x2A) enables the tag.
 *
 * A config-gated version of this was written and is wrong twice over. Beyond the latch, LEDs,
 * buttons, the buzzer and sensors claim their configured pins during their own init, before
 * config load reaches opendisplay_nfc_apply_config() -- so a config that enabled NFC could hand
 * these pads to a peripheral first and start NFCT over it.
 *
 * The choice is therefore made at flash time, by which image is built: with NFCT in, these two
 * pins are the tag's; without it they are free. That is the reset-time configuration this SoC
 * offers, and a separate image is the only way to change it.
 *
 * The refusal belongs here because the failure it replaces is silent: gpio_pin_set() on an
 * NFC-owned pad returns success and drives nothing, the same no-op recorded in
 * docs/FOLLOWUPS.md section 8. Every consumer -- GPIO, I2C, SPI, battery -- reaches a pin through
 * this one function. */
#define OD_NFC_ANTENNA_PORT 0u
#define OD_NFC_ANTENNA_PIN_A 9u
#define OD_NFC_ANTENNA_PIN_B 10u

static bool pin_is_nfc_antenna(uint8_t port, uint8_t pin)
{
	return port == OD_NFC_ANTENNA_PORT
	       && (pin == OD_NFC_ANTENNA_PIN_A || pin == OD_NFC_ANTENNA_PIN_B);
}
#endif

bool od_pin_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out)
{
	uint8_t port;
	uint8_t pin;

	if (!od_pin_decode_absolute(cfg, 1u, &port, &pin) || !od_gpio_port_ready(port)) {
		return false;
	}
#if IS_ENABLED(CONFIG_NFC_T2T_NRFXLIB)
	if (pin_is_nfc_antenna(port, pin)) {
		return false;
	}
#endif

	*port_out = port;
	*pin_out = pin;
	return true;
}
