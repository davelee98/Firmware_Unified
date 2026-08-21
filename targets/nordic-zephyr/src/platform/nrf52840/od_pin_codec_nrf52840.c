#include "od_gpio.h"
#include "od_pin_codec.h"
#include "opendisplay_nfc.h"

#include <zephyr/sys/util.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_OD_PLATFORM_NRF52840),
	     "nRF52840 pin codec compiled for the wrong platform");
BUILD_ASSERT(!IS_ENABLED(CONFIG_OD_PLATFORM_NRF54),
	     "nRF52840 and nRF54 platform selections are mutually exclusive");

#if IS_ENABLED(CONFIG_NFC_T2T_NRFXLIB)
/* P0.09 and P0.10 are this SoC's NFC antenna pair.
 *
 * OWNERSHIP FOLLOWS THE CONFIG, NOT THE BUILD. The tag is started only by an enabled nfc_config
 * (0x2A) naming a SoC IC, so a device with no NFC block configured leaves these two pins to
 * whatever else the config assigns them to. Keying this on CONFIG_NFC_T2T_NRFXLIB instead would
 * hold both pins hostage on every board that merely *could* carry a tag.
 *
 * While the tag IS running the pins cannot serve a GPIO assignment, and the refusal has to happen
 * here: gpio_pin_set() on an NFC-owned pad returns success and drives nothing, the same silent
 * no-op recorded in docs/FOLLOWUPS.md section 8 for the nRF54 board pins. Every consumer -- GPIO,
 * I2C, SPI, battery -- reaches a pin through this one function, so one test covers them all.
 *
 * WHAT THIS DOES NOT DO: flip the UICR NFCPINS latch. That is read at reset and decides whether
 * the pads are NFC or GPIO at the hardware level; changing it needs a UICR write and a reboot.
 * See docs/FOLLOWUPS.md. */
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
	if (opendisplay_nfc_owns_antenna() && pin_is_nfc_antenna(port, pin)) {
		return false;
	}
#endif

	*port_out = port;
	*pin_out = pin;
	return true;
}
