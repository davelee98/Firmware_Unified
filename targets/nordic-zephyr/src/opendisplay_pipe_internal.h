/* opendisplay_pipe_internal.h -- the few things opendisplay_pipe.c owns that other target files
 * legitimately need, so neither has to duplicate them.
 *
 * NOT a general escape hatch. Each entry here exists because the alternative was a second copy of
 * a fact with one owner: the session singleton, and the silicon identity that feeds both the KDF
 * and the auth proof. A second copy of either is a divergence waiting to happen -- the device id
 * in particular is WIRE-VISIBLE, so a different packing is a different device to the host.
 */

#ifndef OPENDISPLAY_PIPE_INTERNAL_H
#define OPENDISPLAY_PIPE_INTERNAL_H

#include "od_session.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This target's one session. Never memset by a caller: od_session_clear() preserves the crypto
 * slot, which is configuration rather than session state. */
struct od_session *od_pipe_session(void);

/* The four device-id bytes: the low 32 bits of the 64-bit hwinfo id, big-endian. */
void od_pipe_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN]);

/* The live connection generation -- this target's frame identity, incremented on every close. A
 * queued frame carries the generation that produced it, so od_txq and od_rxq can both discard work
 * belonging to a connection that has gone. */
uint32_t od_pipe_conn_gen(void);


#ifdef __cplusplus
}
#endif

#endif /* OPENDISPLAY_PIPE_INTERNAL_H */
