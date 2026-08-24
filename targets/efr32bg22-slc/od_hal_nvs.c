/* NVM3 implementation of od_hal_nvs. See shared/hal/od_hal_nvs.h for the contract.
 *
 * NO CACHE AND NO STAGING BUFFER, which is the whole reason the seam takes an offset:
 * nvm3_readPartialData() serves any span of the stored object straight into the caller's
 * memory. On a 32 KB part with ~10.5 KB of heap, a copy of the record would cost more than
 * everything else this file does.
 */

#include "od_hal_nvs.h"

#include <stddef.h>

#include "nvm3_default.h"
#include "sl_status.h"

#define OD_NVM3_CONFIG_KEY ((nvm3_ObjectKey_t)0x0F4401u)

/* nvm3_defaultHandle is statically bound to nvm3_defaultHandleData, so it is never NULL and a
 * pointer test says nothing about whether the instance was ever opened. hasBeenOpened is the
 * real answer, and nvm3_initDefault() is idempotent, so this is also the on-demand init the
 * seam requires of every entry point. */
int od_hal_nvs_init(void)
{
  if (nvm3_defaultHandle->hasBeenOpened) {
    return OD_HAL_NVS_OK;
  }
  return (nvm3_initDefault() == SL_STATUS_OK) ? OD_HAL_NVS_OK : OD_HAL_NVS_EIO;
}

int od_hal_nvs_size(uint32_t *len_out)
{
  uint32_t obj_type = 0;
  size_t obj_len = 0;
  sl_status_t sc;

  if (len_out == NULL) {
    return OD_HAL_NVS_EIO;
  }
  *len_out = 0u;
  if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
    return OD_HAL_NVS_EIO;
  }

  sc = nvm3_getObjectInfo(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &obj_type, &obj_len);
  if (sc == ECODE_NVM3_ERR_KEY_NOT_FOUND) {
    return OD_HAL_NVS_ENOENT;
  }
  if (sc != SL_STATUS_OK) {
    return OD_HAL_NVS_EIO;
  }

  *len_out = (uint32_t)obj_len;
  return OD_HAL_NVS_OK;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
  uint32_t stored = 0u;
  int rc;
  sl_status_t sc;

  if (buf == NULL) {
    return OD_HAL_NVS_EIO;
  }

  /* nvm3_readPartialData rejects an out-of-range span itself, but not with a code that
   * separates "past the end" from a medium fault, and the caller's recovery differs. A
   * zero-length read is still a question about the record, so it is answered here rather
   * than dismissed as an empty span. */
  rc = od_hal_nvs_size(&stored);
  if (rc != OD_HAL_NVS_OK) {
    return rc;
  }
  /* Subtraction, so a large offset cannot wrap the sum past the record. */
  if (offset > stored || len > stored - offset) {
    return OD_HAL_NVS_E2BIG;
  }
  if (len == 0u) {
    return OD_HAL_NVS_OK;
  }

  sc = nvm3_readPartialData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, buf,
                            (size_t)offset, (size_t)len);
  return (sc == SL_STATUS_OK) ? OD_HAL_NVS_OK : OD_HAL_NVS_EIO;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
  sl_status_t sc;

  if (record == NULL || len == 0u) {
    return OD_HAL_NVS_EIO;
  }
  if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
    return OD_HAL_NVS_EIO;
  }

  sc = nvm3_writeData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, record, (size_t)len);
  return (sc == SL_STATUS_OK) ? OD_HAL_NVS_OK : OD_HAL_NVS_EIO;
}

int od_hal_nvs_erase(void)
{
  sl_status_t sc;

  if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
    return OD_HAL_NVS_EIO;
  }

  sc = nvm3_deleteObject(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY);
  /* Deleting an already-absent record has the requested postcondition. */
  if (sc == SL_STATUS_OK || sc == ECODE_NVM3_ERR_KEY_NOT_FOUND) {
    return OD_HAL_NVS_OK;
  }
  return OD_HAL_NVS_EIO;
}
