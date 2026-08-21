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
OD_STATIC_ASSERT(OD_NFC_READ_MAX == 218u, "NFC read cap is wire-visible");

/* A WIRE CONSTANT, not a target tunable. The canonical header documents NFC_ERR_BAD_TOTAL_LEN as
 * "total_len == 0 or > 512" and the client enforces the same number, so unlike OD_CONFIG_MAX_SIZE
 * this one may not become per-target: a cap a host cannot interrogate is a divergence it cannot
 * work around. */
#define OD_NFC_ASSEMBLY_MAX 512u
OD_STATIC_ASSERT(OD_NFC_ASSEMBLY_MAX == 512u, "NFC assembly limit is the wire's, not the target's");

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
