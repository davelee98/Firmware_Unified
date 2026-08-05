#include "nrf54_zephyr_compat.h"

#include <zephyr/kernel.h>

void od_msleep(int32_t ms)
{
	k_msleep(ms);
}

uint32_t od_uptime_get_32(void)
{
	return k_uptime_get_32();
}

void od_busy_wait(uint32_t usec)
{
	k_busy_wait(usec);
}
