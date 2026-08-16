/* zephyr/kernel.h -- host stand-in, the sliver of it the PIPE-write module uses.
 *
 * Two primitives, between the two Nordic sources tests/host compiles. k_msleep() is the 20 ms
 * opendisplay_pipe_write.cpp waits for a queued END ack to reach the air before a refresh blocks
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

/* Milliseconds since boot. Settable, so a test can pin a timestamp rather than race one. */
extern uint32_t fake_k_uptime_ms;
uint32_t k_uptime_get_32(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_TEST_FAKE_ZEPHYR_KERNEL_H */
