#ifndef OD_ZEPHYR_COMPAT_H
#define OD_ZEPHYR_COMPAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void od_msleep(int32_t ms);

#ifdef __cplusplus
}
#endif

#endif
