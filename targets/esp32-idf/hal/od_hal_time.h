/* od_hal_time -- monotonic clock and bounded waits.
 *
 * Signatures are docs/SHARED_API_DESIGN.md § od_hal_time verbatim ("already exists in embryo":
 * Firmware_NRF54/src/nrf54_zephyr_compat.h is the donor). Written to that contract now so the
 * eventual promotion to shared/hal is a repoint rather than a rewrite -- the same bet
 * od_hal_nvs took.
 *
 * These replace Arduino's millis()/delay()/delayMicroseconds() in TARGET code. They do NOT
 * replace the shim's copies, which stay: two vendored libraries link against `delay` and
 * `millis` by name (bb_epaper declares `void delay(long)` unmangled; FastEPD's arduino_io.inl
 * has an extern millis() with 19 call sites), so those symbols are pinned by third_party/ and
 * cannot leave with the app code. See compat/SHIM_BUDGET, endgame note.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Milliseconds since boot. Free-running 32-bit, wraps at ~49.7 days -- callers compare with
 * unsigned subtraction, which is wrap-safe, and that is the semantics every existing call site
 * was written against under Arduino's millis(). The truncation is therefore load-bearing, not
 * an accident of the return type. */
uint32_t od_hal_uptime_ms(void);

/* Bounded wait, NOT a scheduler yield and NOT a way to wait out a panel refresh (see
 * SHARED_API_DESIGN § od_hal_time). Sleeps the calling task: on ESP32 this is vTaskDelay, so
 * other tasks run.
 *
 * Rounds UP to whole ticks. At the default 100 Hz tick a naive vTaskDelay(pdMS_TO_TICKS(5))
 * rounds DOWN to zero ticks and returns immediately, turning a deliberate settle into a
 * busy-spin -- which is exactly the defect the shim's delay() was written to avoid, and this
 * inherits its arithmetic unchanged. */
void od_hal_delay_ms(uint32_t ms);

/* Busy-wait, microsecond scale. Does not yield -- this is for hardware timing (the D-FF clock
 * pulse's 50 us setup/hold), so yielding would defeat it. Keep the argument small. */
void od_hal_delay_us(uint32_t us);

#ifdef __cplusplus
}
#endif
