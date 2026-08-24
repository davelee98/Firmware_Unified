#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config_asm.h" /* OD_CONFIG_MAX_SIZE, defined there and overridden per target by shared/profiles.cmake */

bool initConfigStorage(void);

bool saveConfig(uint8_t *config_data, uint32_t len);

bool loadConfig(uint8_t *config_data, uint32_t *len);

bool clearStoredConfig(void);

/* The sole config staging object. CONFIG_WRITE drives it directly; storage and parsing share its
 * buffer so the 32 KB target never carries a second 2 KB blob. */
struct od_config_asm *opendisplay_config_assembler(void);

#endif
