#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config_asm.h"

#define MAX_CONFIG_SIZE 2048

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t crc;
  uint32_t data_len;
  uint8_t data[MAX_CONFIG_SIZE];
} opendisplay_config_storage_t;

bool initConfigStorage(void);

bool saveConfig(uint8_t *config_data, uint32_t len);

bool loadConfig(uint8_t *config_data, uint32_t *len);

bool clearStoredConfig(void);

uint32_t calculateConfigCRC(uint8_t *data, uint32_t len);

/* The sole config staging object. CONFIG_WRITE drives it directly; storage and parsing share its
 * buffer so the 32 KB target never carries a second 2 KB blob. */
struct od_config_asm *opendisplay_config_assembler(void);

#endif
