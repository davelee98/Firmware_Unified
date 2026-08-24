/* zephyr/kernel.h -- host stand-in, the sliver of it the PIPE-write module uses.
 *
 * Two primitives, between the two Nordic sources tests/host compiles. k_msleep() is the 20 ms
 * the shared PIPE path waits for a queued END ack to reach the air before a refresh blocks
 * the thread -- on the host a counter, so a test can assert the barrier happened without spending
 * the time. k_uptime_get_32() is settable, so a timestamp can be pinned rather than raced.
 */

#ifndef OD_TEST_FAKE_ZEPHYR_KERNEL_H
#define OD_TEST_FAKE_ZEPHYR_KERNEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Total milliseconds asked for since the last fake_zephyr_reset(). */
extern uint32_t fake_k_sleep_ms;
void fake_zephyr_reset(void);

void k_msleep(int32_t ms);

/* Busy-wait, for the bit-banged I2C engine. The suite that uses it checks EDGE ORDER, not
 * timing, so an implementation may discard the argument -- but it must exist, or the engine
 * compiles with an implicit declaration and the delays vanish silently. */
void k_busy_wait(uint32_t us);

/* Milliseconds since boot. Settable, so a test can pin a timestamp rather than race one. */
extern uint32_t fake_k_uptime_ms;
uint32_t k_uptime_get_32(void);

/* CMSIS, not the kernel -- but it reaches a target source through this header, so the stand-in
 * lives here too. Counted rather than performed, obviously. */
extern unsigned fake_nvic_resets;
void NVIC_SystemReset(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_TEST_FAKE_ZEPHYR_KERNEL_H */
