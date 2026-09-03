#ifndef OPENDISPLAY_DEVICE_FLAGS_H
#define OPENDISPLAY_DEVICE_FLAGS_H

/* system_config.device_flags — shared with nRF52840 Firmware/src/main.h */

/* Only the bit this target acts on. Bits 0-4 (PWR_PIN, XIAOINIT, WS_PP_INIT, BATTERY_LATCH,
 * PWR_LATCH_DFF) are ESP32 behaviours with no Nordic implementation; they were defined here and
 * never read. The wire meaning of every bit is opendisplay_structs.h's, not this file's. */
#define DEVICE_FLAG_CHANNEL_SOUNDING  (1u << 5)

#endif
