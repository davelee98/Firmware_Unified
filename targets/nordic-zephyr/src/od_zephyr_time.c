#include "od_zephyr_compat.h"
#include "od_hal_time.h"

#include <zephyr/kernel.h>

void od_msleep(int32_t ms)
{
	k_msleep(ms);
}

uint32_t od_hal_uptime_ms(void)
{
	return k_uptime_get_32();
}

void od_hal_delay_us(uint32_t usec)
{
	k_busy_wait(usec);
}
