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

/* Implemented by the target. Applies the activity stamp and the auth-abuse policy, both gated on
 * this tag still owning the link. The two have DIFFERENT origin scopes -- the stamp applies to
 * every origin, the abuse run is BLE-only -- and getting the first wrong stops an active LAN
 * client's idle clock and disconnects it mid-session. */
void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_H */
