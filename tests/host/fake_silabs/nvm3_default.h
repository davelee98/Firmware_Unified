/* Enough of the NVM3 surface for targets/efr32bg22-slc/opendisplay_config_storage.c to compile and
 * run on the host. One object slot, because that file stores exactly one key.
 *
 * ECODE_NVM3_ERR_KEY_NOT_FOUND is a macro aliased to SL_STATUS_NOT_FOUND in Simplicity SDK
 * 2025.12.2 (nvm3_generic.h:66). Reproduced as an alias rather than a distinct value so the
 * storage file's `sc == ECODE_NVM3_ERR_KEY_NOT_FOUND` comparison is tested in the shape it ships.
 */

#ifndef OD_TEST_FAKE_SILABS_NVM3_DEFAULT_H
#define OD_TEST_FAKE_SILABS_NVM3_DEFAULT_H

#include "sl_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t nvm3_ObjectKey_t;
typedef struct nvm3_Handle nvm3_Handle_t;

#define ECODE_NVM3_ERR_KEY_NOT_FOUND SL_STATUS_NOT_FOUND
#define NVM3_OBJECTTYPE_DATA 0u

extern nvm3_Handle_t *nvm3_defaultHandle;

sl_status_t nvm3_writeData(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                           const void *value, size_t len);
sl_status_t nvm3_readPartialData(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                                 void *value, size_t offset, size_t len);
sl_status_t nvm3_getObjectInfo(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                               uint32_t *type, size_t *len);
sl_status_t nvm3_deleteObject(nvm3_Handle_t *h, nvm3_ObjectKey_t key);

/* ---- test control ---- */

#define NVM3_FAKE_MAX_OBJECT 2112u

void nvm3_fake_reset(void);
/* Detach the backing store the way an uninitialised NVM3 instance would present. */
void nvm3_fake_set_mounted(bool mounted);

extern sl_status_t nvm3_fake_write_status;
extern sl_status_t nvm3_fake_read_status;
extern sl_status_t nvm3_fake_delete_status;

/* The exact image handed to nvm3_writeData, so a test can assert the record layout the device
 * would actually persist rather than re-deriving it. */
extern uint8_t nvm3_fake_last_write[NVM3_FAKE_MAX_OBJECT];
extern size_t nvm3_fake_last_write_len;
extern unsigned nvm3_fake_writes;
extern unsigned nvm3_fake_deletes;

/* Flash contents, for corruption injection. */
extern bool nvm3_fake_present;
extern uint8_t nvm3_fake_object[NVM3_FAKE_MAX_OBJECT];
extern size_t nvm3_fake_object_len;

#endif /* OD_TEST_FAKE_SILABS_NVM3_DEFAULT_H */
