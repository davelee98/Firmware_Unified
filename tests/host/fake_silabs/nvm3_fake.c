/* One-object NVM3 stand-in. Holds bytes; knows nothing about the config record's layout. */

#include "nvm3_default.h"

#include <string.h>

static nvm3_Handle_t s_handle;
nvm3_Handle_t *nvm3_defaultHandle = &s_handle;
static bool s_mountable = true;

sl_status_t nvm3_fake_write_status;
sl_status_t nvm3_fake_read_status;
sl_status_t nvm3_fake_delete_status;

uint8_t nvm3_fake_last_write[NVM3_FAKE_MAX_OBJECT];
size_t nvm3_fake_last_write_len;
unsigned nvm3_fake_writes;
unsigned nvm3_fake_deletes;

bool nvm3_fake_present;
uint8_t nvm3_fake_object[NVM3_FAKE_MAX_OBJECT];
size_t nvm3_fake_object_len;

void nvm3_fake_reset(void)
{
    nvm3_defaultHandle = &s_handle;
    s_mountable = true;
    s_handle.hasBeenOpened = true;
    nvm3_fake_write_status = SL_STATUS_OK;
    nvm3_fake_read_status = SL_STATUS_OK;
    nvm3_fake_delete_status = SL_STATUS_OK;
    memset(nvm3_fake_last_write, 0, sizeof nvm3_fake_last_write);
    nvm3_fake_last_write_len = 0u;
    nvm3_fake_writes = 0u;
    nvm3_fake_deletes = 0u;
    nvm3_fake_present = false;
    memset(nvm3_fake_object, 0, sizeof nvm3_fake_object);
    nvm3_fake_object_len = 0u;
}

void nvm3_fake_set_mounted(bool mounted)
{
    s_mountable = mounted;
    s_handle.hasBeenOpened = mounted;
}

void nvm3_fake_set_opened(bool opened)
{
    s_handle.hasBeenOpened = opened;
}

/* The SDK refuses every operation on a handle that was never opened. Modelled so that an adapter
 * which stopped initialising on demand fails here instead of silently working. */
static bool handle_usable(const nvm3_Handle_t *h)
{
    return h != NULL && h->hasBeenOpened;
}

sl_status_t nvm3_initDefault(void)
{
    if (!s_mountable) {
        return SL_STATUS_FAIL;
    }
    s_handle.hasBeenOpened = true;
    return SL_STATUS_OK;
}

sl_status_t nvm3_writeData(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                           const void *value, size_t len)
{
    (void)key;
    if (!handle_usable(h)) {
        return SL_STATUS_NOT_INITIALIZED;
    }
    if (value == NULL || len > NVM3_FAKE_MAX_OBJECT) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    ++nvm3_fake_writes;
    /* Record the attempt even when the write is scripted to fail: a caller that computed the
     * image before failing is a different defect from one that never built it. */
    memcpy(nvm3_fake_last_write, value, len);
    nvm3_fake_last_write_len = len;
    if (nvm3_fake_write_status != SL_STATUS_OK) {
        return nvm3_fake_write_status;
    }
    memcpy(nvm3_fake_object, value, len);
    nvm3_fake_object_len = len;
    nvm3_fake_present = true;
    return SL_STATUS_OK;
}

sl_status_t nvm3_getObjectInfo(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                               uint32_t *type, size_t *len)
{
    (void)key;
    if (!handle_usable(h)) {
        return SL_STATUS_NOT_INITIALIZED;
    }
    if (type == NULL || len == NULL) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    if (!nvm3_fake_present) {
        return ECODE_NVM3_ERR_KEY_NOT_FOUND;
    }
    *type = NVM3_OBJECTTYPE_DATA;
    *len = nvm3_fake_object_len;
    return SL_STATUS_OK;
}

sl_status_t nvm3_readPartialData(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                                 void *value, size_t offset, size_t len)
{
    (void)key;
    if (!handle_usable(h)) {
        return SL_STATUS_NOT_INITIALIZED;
    }
    if (value == NULL) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    if (!nvm3_fake_present) {
        return ECODE_NVM3_ERR_KEY_NOT_FOUND;
    }
    if (nvm3_fake_read_status != SL_STATUS_OK) {
        return nvm3_fake_read_status;
    }
    if (offset > nvm3_fake_object_len || len > nvm3_fake_object_len - offset) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    memcpy(value, &nvm3_fake_object[offset], len);
    return SL_STATUS_OK;
}

sl_status_t nvm3_deleteObject(nvm3_Handle_t *h, nvm3_ObjectKey_t key)
{
    (void)key;
    if (!handle_usable(h)) {
        return SL_STATUS_NOT_INITIALIZED;
    }
    ++nvm3_fake_deletes;
    if (nvm3_fake_delete_status != SL_STATUS_OK) {
        return nvm3_fake_delete_status;
    }
    if (!nvm3_fake_present) {
        return ECODE_NVM3_ERR_KEY_NOT_FOUND;
    }
    nvm3_fake_present = false;
    nvm3_fake_object_len = 0u;
    return SL_STATUS_OK;
}
