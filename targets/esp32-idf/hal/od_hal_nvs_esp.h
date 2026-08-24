/* Where this target's config record lives in NVS, and how large it can get.
 *
 * Private to hal/od_hal_nvs.c and hal/od_hal_nvs_secure.c. Both address the same entry, and a
 * namespace/key pair spelled twice is a pair that can drift.
 */

#ifndef OD_HAL_NVS_ESP_H
#define OD_HAL_NVS_ESP_H

#include "od_config_asm.h"   /* OD_CONFIG_MAX_SIZE */

/* One namespace, one key. The blob is opaque here; its framing belongs to the core. */
#define OD_NVS_NAMESPACE "opendisplay"
#define OD_NVS_KEY       "config"

/* Largest record this target stores: the 16-byte config header plus the payload cap. */
#define OD_HAL_NVS_MAX_RECORD (16u + OD_CONFIG_MAX_SIZE)

/* Discard od_hal_nvs.c's cached copy of the record. Anything that writes this NVS entry from
 * another translation unit must call it, or the next read serves bytes the medium no longer
 * holds. */
void od_hal_nvs_esp_cache_drop(void);

#endif /* OD_HAL_NVS_ESP_H */
