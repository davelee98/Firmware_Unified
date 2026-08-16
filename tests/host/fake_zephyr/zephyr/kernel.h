/* zephyr/kernel.h -- host stand-in, the sliver of it the PIPE-write module uses.
 *
 * opendisplay_pipe_write.cpp calls exactly one kernel primitive: k_msleep(), the 20 ms it waits
 * for a queued END ack to reach the air before a refresh blocks the thread. On the host that is a
 * counter, so a test can assert the barrier happened without spending the time.
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

#ifdef __cplusplus
}
#endif

#endif /* OD_TEST_FAKE_ZEPHYR_KERNEL_H */
