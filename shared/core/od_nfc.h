/* od_nfc.h -- the shared CMD_NFC_ENDPOINT (0x0083) machine.
 *
 * One state machine for both capability arms. Targets supply only the tag itself, through
 * od_nfc_app.h; everything on the wire -- sub-command parsing, record-type validation, length
 * bounds, chunk assembly, ownership, error codes and reply framing -- is here.
 *
 * BOTH SYMBOLS EXIST UNDER EITHER ARM. od_core_reset() names the reset and, from step 8, dispatch
 * names the frame entry point, so a capability-off target must still link them. What
 * OD_CAP_NFC=0 removes is the state and the seam reference, not the surface.
 */
#ifndef OD_NFC_H
#define OD_NFC_H

#include "od_caps.h"
#include "od_cmd.h"
#include "od_session.h"
#include "od_span.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

/* The read response is [status][cmd][0x80][rec_type][len:2][data], and od_session caps the whole
 * sealed frame. Derived rather than written down: 218 is wire-visible, so it must move if the
 * envelope ever does, and must not move quietly otherwise. */
#define OD_NFC_READ_MAX ((uint16_t)(OD_SESSION_PAYLOAD_MAX - 4u))

/* A WIRE CONSTANT, not a target tunable. The canonical header documents NFC_ERR_BAD_TOTAL_LEN as
 * "total_len == 0 or > 512" and the client enforces the same number, so unlike OD_CONFIG_MAX_SIZE
 * this one may not become per-target: a cap a host cannot interrogate is a divergence it cannot
 * work around. */
#define OD_NFC_ASSEMBLY_MAX 512u

/* THE ASSERTS FOR BOTH CONSTANTS LIVE IN od_nfc.c, not here. Under -std=c99 -- which is how
 * shared/ is compiled for the host -- OD_STATIC_ASSERT falls back to a typedef named only by
 * __LINE__, so two headers carrying an assert on the same line collide in any translation unit
 * that includes both. This header hit that against an existing one the moment dispatch began
 * including it. The protocol header that defines the macro is a byte-for-byte copy of the
 * canonical one and is not ours to fix, so the asserts move instead of the macro. */

#ifdef __cplusplus
extern "C" {
#endif

/* Handle one 0x0083 frame. `body` is the payload after the two command bytes. */
od_cmd_result_t od_nfc_frame(const od_cmd_ctx_t *ctx, od_span_t body);

/* Drop any partial assembly. Called by od_core_reset() as part of a teardown, and a no-op under
 * OD_CAP_NFC=0. */
void od_nfc_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_NFC_H */
