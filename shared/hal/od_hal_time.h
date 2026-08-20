/* od_hal_time.h -- ambient uptime and short busy-wait target seam. */

#ifndef OD_HAL_TIME_H
#define OD_HAL_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ambient monotonic time for consumers such as logging. Milliseconds since the current boot,
 * converted in the target's full native clock domain and then narrowed modulo 2^32. This is not
 * wall time and must not jump due to clock synchronization. The result wraps about every 49.7
 * days; elapsed-time callers must compare it with unsigned subtraction.
 *
 * Shared policy that already accepts now_ms as an argument must keep doing so. In particular,
 * session, watchdog and TXQ policy remains directly testable without installing a fake clock.
 * Do not replace those explicit parameters with calls to this ambient clock. */
uint32_t od_hal_uptime_ms(void);

/* Busy-wait for short hardware timing. This does not yield; keep the argument small. */
void od_hal_delay_us(uint32_t us);

/* od_hal_delay_ms()/bounded sleep is deliberately absent. ESP32 currently takes uint32_t and
 * rounds every request up to at least one RTOS tick; Nordic's int32_t k_msleep wrapper treats
 * non-positive values as already expired. Naming, signedness, zero behavior and the positive-delay
 * round-up rule must be reconciled in a separate decision before a sleep function is added here. */

#ifdef __cplusplus
}
#endif

#endif
