#ifndef OPENDISPLAY_IDLE_WAKE_H
#define OPENDISPLAY_IDLE_WAKE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback/ISR-safe notification that asynchronous BLE work is ready for main. */
void opendisplay_idle_wake(void);

/* Main-thread only. True means a latched event interrupted the bounded wait. */
bool opendisplay_idle_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* OPENDISPLAY_IDLE_WAKE_H */
