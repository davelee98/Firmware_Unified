#ifndef OPENDISPLAY_NFC_H
#define OPENDISPLAY_NFC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_config;

/* Start/stop SoC NFCT from nfc_config (0x2A). Safe to call on reload. */
void opendisplay_nfc_apply_config(const struct od_config *cfg);

/* True while the SoC NFCT tag is running, which is the only state in which NFC owns the antenna
 * pins. Driven entirely by config: an enabled nfc_config (0x2A) naming a SoC IC turns it on, its
 * absence leaves it off. The pin decoder asks so an unconfigured tag does not hold pins hostage.
 * Always false where the tag is not built. */
bool opendisplay_nfc_owns_antenna(void);
/* Flush debounced NFC field events (call from main BLE process loop). */
void opendisplay_nfc_process(void);

#ifdef __cplusplus
}
#endif

#endif
