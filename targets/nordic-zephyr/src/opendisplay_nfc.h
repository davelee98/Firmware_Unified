#ifndef OPENDISPLAY_NFC_H
#define OPENDISPLAY_NFC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GlobalConfig;

/* Start/stop SoC NFCT from nfc_config (0x2A). Safe to call on reload. */
void opendisplay_nfc_apply_config(const struct GlobalConfig *cfg);
/* Flush debounced NFC field events (call from main BLE process loop). */
void opendisplay_nfc_process(void);

#ifdef __cplusplus
}
#endif

#endif
