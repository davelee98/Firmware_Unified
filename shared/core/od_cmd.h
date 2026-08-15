/* od_cmd.h -- what a handler returns, and what the dispatcher concluded about a frame.
 *
 * SCAFFOLDING, with a shrink schedule. These types exist so dispatch can be written once instead
 * of three times; they are not a permanent public API. As each transfer subsystem is promoted its
 * handlers move behind their own headers and the od_cmd_* declarations here go with them. Do not
 * grow this file into a command registry.
 */

#ifndef OD_CMD_H
#define OD_CMD_H

#include "od_txq.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a handler concluded. Three, not two: a handler can refuse a frame for want of
 * authentication itself, and that is not the same as a NACK. Without the third value a TLS
 * client's refused CONFIG_WRITE stamps activity and holds the exclusive link forever. */
typedef enum {
    OD_CMD_OK,
    OD_CMD_NACK,
    OD_CMD_AUTH_REJECTED
} od_cmd_result_t;

/* What the dispatcher concluded about one inbound frame. The target turns this into policy in
 * od_core_frame_done(); see the table there for which outcomes stamp activity and which move the
 * auth-abuse run. The distinctions are load-bearing, not cosmetic:
 *
 *  - AUTH_CONTROL and DISCOVERY exist because a handshake or a version poll must NOT stamp
 *    activity. Treating either as ACCEPTED lets a discovery poll hold the exclusive link forever.
 *  - AUTH_ESTABLISHED is separate again: it resets the abuse run but still does not stamp.
 *  - AUTH_REQUIRED and CRYPTO_FAILED are separate because only the first advances the abuse run.
 *    od_session already counts a bad tag toward its own three-strike rule; dispatch must not add
 *    a second strike for the same frame.
 *  - CRYPTO_DROPPED is the deliberate silence: a replayed or out-of-window PIPE data frame is
 *    ordinary packet loss, and answering it with a fatal NACK kills the upload.
 */
typedef enum {
    OD_FRAME_ACCEPTED,
    OD_FRAME_HANDLER_NACK,
    OD_FRAME_AUTH_CONTROL,      /* challenge or refused handshake; plaintext, not activity */
    OD_FRAME_AUTH_ESTABLISHED,  /* plaintext success; resets abuse, still not activity */
    OD_FRAME_DISCOVERY,         /* FIRMWARE_VERSION; plaintext, not activity */
    OD_FRAME_AUTH_REQUIRED,     /* answered [0x00][cmd][0xFE] -- gate OR handler */
    OD_FRAME_CRYPTO_FAILED,     /* crypto refusal answered [0x00][cmd][0xFF] */
    OD_FRAME_CRYPTO_DROPPED,    /* PIPE replay/out-of-window: logged, deliberately silent */
    OD_FRAME_REJECTED_FRAME,
    OD_FRAME_UNKNOWN_OPCODE,
    OD_FRAME_STALE_TAG,
    OD_FRAME_DEFERRED           /* the ONLY outcome that does not consume the RX entry */
} od_frame_outcome_t;

/* What one outcome MEANS, as data rather than as a switch in each target. The two targets got this
 * wrong in different ways before it was written down, and the failure is quiet: an outcome that
 * wrongly stamps activity lets a discovery poll hold the exclusive link forever, and one that
 * wrongly does not stops an active client's idle clock and disconnects it mid-session. */
typedef struct {
    bool stamp_activity;   /* refresh the owner idle clock -- EVERY origin */
    bool reset_abuse;      /* clear the consecutive-auth-refusal run -- BLE only */
    bool increment_abuse;  /* advance that run -- BLE only */
    bool consume_rx;       /* false ONLY for DEFERRED: the frame must be re-offered unchanged */
} od_frame_policy_t;

/* Total over od_frame_outcome_t. The caller still owns the ORIGIN SCOPING, which is not in this
 * struct because it is not a property of the outcome: the stamp applies to every origin, the abuse
 * run is BLE-only, and both additionally require the tag to still own the link. */
od_frame_policy_t od_frame_policy(od_frame_outcome_t outcome);

/* Implemented by the target. Applies od_frame_policy() with that scoping. */
void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_H */
