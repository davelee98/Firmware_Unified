#ifndef ENCRYPTION_STATE_H
#define ENCRYPTION_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "structs.h"

/* The parsed configuration this firmware runs on. The SESSION is deliberately absent: it is
 * file-static in od_session_app.cpp and reached through od_session_app_state(), because an
 * exported singleton is one a caller can memset -- which strands its od_hal_crypto key slot. */
extern struct SecurityConfig &securityConfig;
extern bool encryptionInitialized;

#endif
