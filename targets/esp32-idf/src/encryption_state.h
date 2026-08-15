#ifndef ENCRYPTION_STATE_H
#define ENCRYPTION_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "structs.h"
#include "od_session.h"

/* The one BLE session. Session state lives in shared/core/od_session.h; the session key is not in
 * this struct at all -- it lives in an od_hal_crypto slot, so g_session stays trivially zeroable
 * and a memory dump of it yields no key material. Never memset it: od_session_clear() is the only
 * teardown, because it also releases the slot and preserves the slot index. */
extern struct od_session g_session;

extern struct SecurityConfig &securityConfig;
extern bool encryptionInitialized;

#endif
