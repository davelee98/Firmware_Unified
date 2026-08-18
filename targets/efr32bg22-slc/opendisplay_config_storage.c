#include "opendisplay_config_storage.h"
#include "nvm3_default.h"
#include "nvm3_default_config.h"
#include "sl_status.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define OD_NVM3_CONFIG_KEY ((nvm3_ObjectKey_t)0x0F4401u)
#define CONFIG_STORAGE_MAGIC 0xDEADBEEFu
#define CONFIG_STORAGE_VERSION 1u

#if (MAX_CONFIG_SIZE + 16u) > NVM3_DEFAULT_MAX_OBJECT_SIZE
#error "MAX_CONFIG_SIZE + header exceeds NVM3_DEFAULT_MAX_OBJECT_SIZE"
#endif

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t crc;
  uint32_t data_len;
} od_config_header_t;

_Static_assert(sizeof(od_config_header_t) ==
                   offsetof(opendisplay_config_storage_t, data),
               "config header layout mismatch");

/* Both layouts put their byte buffer at offset 16 and are exactly 2064 bytes in the BG22 profile.
 * During an NVM3 write the four assembler state words become the legacy storage header; the
 * assembler is reset immediately afterwards. This keeps the deployed record format without a
 * second 2 KB staging object. */
typedef union {
  struct od_config_asm assembler;
  opendisplay_config_storage_t record;
} od_config_work_t;

_Static_assert(offsetof(struct od_config_asm, buffer) ==
                   offsetof(opendisplay_config_storage_t, data),
               "config assembler/storage buffer offsets differ");
_Static_assert(sizeof(struct od_config_asm) == sizeof(opendisplay_config_storage_t),
               "config assembler/storage sizes differ");

static od_config_work_t s_cfg;

struct od_config_asm *opendisplay_config_assembler(void)
{
  return &s_cfg.assembler;
}

bool initConfigStorage(void)
{
  return nvm3_defaultHandle != NULL;
}

uint32_t calculateConfigCRC(uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFu;

  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc >>= 1;
      }
    }
  }
  return ~crc;
}

bool saveConfig(uint8_t *config_data, uint32_t len)
{
  size_t header_sz = offsetof(opendisplay_config_storage_t, data);
  size_t total;
  sl_status_t sc;
  uint32_t crc;

  if (config_data == NULL || len > MAX_CONFIG_SIZE || nvm3_defaultHandle == NULL) {
    return false;
  }

  /* CRC and copy before header writes; config_data may alias the shared buffer. */
  crc = calculateConfigCRC(config_data, len);
  if (config_data != s_cfg.record.data) {
    memmove(s_cfg.record.data, config_data, len);
  }

  s_cfg.record.magic = CONFIG_STORAGE_MAGIC;
  s_cfg.record.version = CONFIG_STORAGE_VERSION;
  s_cfg.record.data_len = len;
  s_cfg.record.crc = crc;

  total = header_sz + len;
  sc = nvm3_writeData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &s_cfg.record, total);
  if (sc != SL_STATUS_OK) {
    printf("[OD] nvm3_writeData config key=0x%06lX sc=0x%04lX len=%u\r\n",
           (unsigned long)OD_NVM3_CONFIG_KEY, (unsigned long)sc, (unsigned)total);
  }
  od_config_asm_reset(&s_cfg.assembler);
  return sc == SL_STATUS_OK;
}

bool loadConfig(uint8_t *config_data, uint32_t *len)
{
  od_config_header_t hdr;
  size_t header_sz = offsetof(opendisplay_config_storage_t, data);
  uint32_t obj_type = 0;
  size_t obj_len = 0;
  sl_status_t sc;

  if (config_data == NULL || len == NULL || nvm3_defaultHandle == NULL) {
    return false;
  }

  sc = nvm3_getObjectInfo(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &obj_type, &obj_len);
  if (sc != SL_STATUS_OK) {
    return false;
  }
  if (obj_len < header_sz || obj_len > sizeof(opendisplay_config_storage_t)) {
    return false;
  }

  sc = nvm3_readPartialData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &hdr, 0, header_sz);
  if (sc != SL_STATUS_OK) {
    return false;
  }

  if (hdr.magic != CONFIG_STORAGE_MAGIC) {
    return false;
  }
  if (hdr.data_len > MAX_CONFIG_SIZE || hdr.data_len > obj_len - header_sz) {
    return false;
  }
  if (hdr.data_len > *len) {
    return false;
  }

  if (hdr.data_len > 0u) {
    sc = nvm3_readPartialData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, config_data,
                              header_sz, hdr.data_len);
    if (sc != SL_STATUS_OK) {
      return false;
    }
  }

  if (calculateConfigCRC(config_data, hdr.data_len) != hdr.crc) {
    return false;
  }

  *len = hdr.data_len;
  return true;
}

bool clearStoredConfig(void)
{
  sl_status_t sc;

  if (nvm3_defaultHandle == NULL) {
    return false;
  }
  sc = nvm3_deleteObject(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY);
  /* Deleting an already-absent config has the requested postcondition. */
  return sc == SL_STATUS_OK || sc == ECODE_NVM3_ERR_KEY_NOT_FOUND;
}
