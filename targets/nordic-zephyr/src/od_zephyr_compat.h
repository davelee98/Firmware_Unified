#ifndef OD_ZEPHYR_COMPAT_H
#define OD_ZEPHYR_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void od_msleep(int32_t ms);
ssize_t od_hwinfo_get_device_id(uint8_t *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
