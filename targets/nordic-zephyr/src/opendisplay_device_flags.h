#ifndef OPENDISPLAY_DEVICE_FLAGS_H
#define OPENDISPLAY_DEVICE_FLAGS_H

/* system_config.device_flags — shared with nRF52840 Firmware/src/main.h */

#define DEVICE_FLAG_PWR_PIN           (1u << 0)
#define DEVICE_FLAG_XIAOINIT          (1u << 1)
#define DEVICE_FLAG_WS_PP_INIT        (1u << 2)
#define DEVICE_FLAG_BATTERY_LATCH     (1u << 3)
#define DEVICE_FLAG_PWR_LATCH_DFF     (1u << 4)
#define DEVICE_FLAG_CHANNEL_SOUNDING  (1u << 5)

#endif
