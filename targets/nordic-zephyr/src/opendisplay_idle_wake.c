#include "opendisplay_idle_wake.h"

#include <zephyr/kernel.h>

/* Binary by design: state and queues carry the work, so a burst needs only one wake. */
K_SEM_DEFINE(s_opendisplay_idle_wake, 0, 1);

void opendisplay_idle_wake(void)
{
	k_sem_give(&s_opendisplay_idle_wake);
}

bool opendisplay_idle_wait(uint32_t timeout_ms)
{
	return k_sem_take(&s_opendisplay_idle_wake, K_MSEC(timeout_ms)) == 0;
}
