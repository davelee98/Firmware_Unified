/* The target's owner of shared/core/od_watchdog.h.
 *
 * The shared controller holds no global state and does not log; something on each target has to
 * own the struct and turn the boot report into log lines. This is that, and it is deliberately
 * the whole of it -- no policy lives here.
 *
 * SINGLE CONTEXT. Every function below must be called from the loop task, the one that
 * setup() runs in. od_watchdog is lock-free by design and the ESP-IDF feed only reaches the
 * task that armed (hal/od_hal_wdt.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* For enum od_wdt_phase, which od_watchdog_app_phase() takes. Callers are breadcrumb sites
 * scattered across the panel path; making each of them include the shared header separately
 * would be the same dependency written many times. */
#include "od_watchdog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode the boot, run the strike counter, log what happened. Call once, early in setup(),
 * before anything can touch the panel. */
void od_watchdog_app_boot(void);

/* Arm the hardware watchdog and log which of arm/inherit/disabled/unsupported happened. Call
 * once, after od_watchdog_app_boot() and before the boot panel path. */
void od_watchdog_app_arm(void);

/* Prove forward progress. Call from loop(), and from any cooperative wait that returns. */
void od_watchdog_app_service(void);

/* True when this boot must skip ALL panel work to stay reachable over BLE.
 *
 * Callers must BRANCH on this, not merely log it: three consecutive watchdog resets mean the
 * panel path is what is wedging, and a fourth attempt at it is another reset. */
bool od_watchdog_app_safe_mode(void);

/* Record the phase about to be entered, so the boot after a watchdog reset can name the wait
 * that wedged. Takes an enum od_wdt_phase value. */
void od_watchdog_app_phase(uint8_t phase);

#ifdef __cplusplus
}
#endif
