#ifndef NRF54_ZEPHYR_COMPAT_H
#define NRF54_ZEPHYR_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void od_msleep(int32_t ms);
uint32_t od_uptime_get_32(void);
void od_busy_wait(uint32_t usec);
ssize_t od_hwinfo_get_device_id(uint8_t *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
