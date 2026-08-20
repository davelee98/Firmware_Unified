#ifndef OD_HAL_SLEEP_H
#define OD_HAL_SLEEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded task sleep, kept target-private until its contract is reconciled with Nordic's signed
 * k_msleep wrapper. Positive delays round up to whole ticks and never become a zero-tick wait.
 * A zero argument retains the deployed ESP32 behavior and sleeps for one tick. */
void od_hal_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
