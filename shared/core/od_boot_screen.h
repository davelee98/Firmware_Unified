#ifndef OD_BOOT_SCREEN_H
#define OD_BOOT_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "od_config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct od_boot_bufs {
  uint8_t *row;
  size_t row_len;
  uint8_t *qr;
  size_t qr_len;
};

bool od_boot_screen_render(const struct od_config *cfg,
                           const struct SecurityConfig *security,
                           const struct od_boot_bufs *bufs);

#ifdef __cplusplus
}
#endif

#endif
