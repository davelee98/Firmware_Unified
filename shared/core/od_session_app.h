/* od_session_app.h -- the target's half of the session, as a link-time seam.
 *
 * od_session deliberately promoted the ALGORITHMS and left four things behind, because they cannot
 * be shared: which object holds the session, where the security config comes from, what time it
 * is, and what this device's identity is. Shared egress and the dispatcher need all four. Without
 * a seam each would have to be threaded through every call as parameters, and the dispatcher would
 * carry a session pointer it has no business owning.
 *
 * THE REPORT CALLBACK IS THE POINT OF THE FILE. Every target logged from inside the session code
 * before the promotion, and shared/ may not include a target log header (CLAUDE.md, "the one
 * rule"). So the core calls back with what happened and the target decides how to say it -- which
 * also keeps the per-site five-second rate limiting on the target, where the clock is.
 *
 * IT IS CALLED BEFORE THE CALLER ACTS ON THE RESULT, and on the PIPE path that ordering is the
 * whole point: a replay or out-of-window 0x81 draws no response, so if telemetry came after the
 * decision to stay silent it would disappear on exactly the path that produces it routinely. That
 * regression has already happened once here.
 *
 * CONTEXT: single loop/main task, like od_txq and od_session. No locks.
 */

#ifndef OD_SESSION_APP_H
#define OD_SESSION_APP_H

#include "od_session.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which session operation produced the report. `result` is that operation's own enum, widened to
 * int because the four do not share a type -- the target switches on `op` first. */
enum od_session_app_op {
    OD_SESSION_APP_ALIVE,   /* od_session_alive()        -> bool, 1 = still live */
    OD_SESSION_APP_AUTH,    /* od_session_authenticate() -> enum od_session_auth */
    OD_SESSION_APP_OPEN,    /* od_session_open()         -> enum od_session_open */
    OD_SESSION_APP_SEAL     /* od_session_seal()         -> enum od_session_seal */
};

/* The one session. Never NULL. The target owns the storage; shared code only ever reaches it
 * through here, and MUST NOT memset it -- od_session_clear() is the only teardown, because it also
 * releases the HAL key slot and preserves the slot index. */
struct od_session *od_session_app_state(void);

/* The parsed security config, or NULL when none is stored. NULL is a legitimate protocol state
 * (AUTH_STATUS_NOT_CONFIG), not an error. */
const struct SecurityConfig *od_session_app_security(void);

/* Milliseconds since boot, from the same clock every other session timestamp uses. Mixing clocks
 * breaks the wrap-safe unsigned subtractions the timeout and the challenge window rely on. */
uint32_t od_session_app_now_ms(void);

/* The four device-identity bytes that feed both the KDF and the auth proof. Wire-visible: a
 * different packing is a different device to the host. */
void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN]);

/* Tell the target what happened. `cmd` is the opcode in play, or 0 where none applies. `report`
 * may be NULL. MUST NOT emit a frame, mutate the session, or block -- it is a logging seam, and a
 * target that answers the wire from here reintroduces exactly the "handler that sends behind the
 * dispatcher's back" problem od_session_authenticate was shaped to avoid. */
void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report);

#ifdef __cplusplus
}
#endif

#endif /* OD_SESSION_APP_H */
