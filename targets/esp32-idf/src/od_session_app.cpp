/* od_session_app.cpp -- this target's half of the session seam (shared/core/od_session_app.h).
 *
 * Four accessors and a log sink. The accessors exist because shared egress and the dispatcher need
 * the session, the config, the clock and the device id, and none of the four can be shared. The
 * log sink exists because shared/ may not include od_log.h -- so the core reports what happened
 * and the wording, the level and the throttling stay here.
 */

#include "od_session_app.h"
#include "od_txq.h"   /* od_reply_t / od_radio_result_t, for the drop seam at the end */

#include "encryption_state.h"
#include "od_hal_time.h"
#include "od_log.h"

#include "opendisplay_protocol.h"

/* C++ linkage, matching its definition in encryption.cpp -- this is a target function, not part
 * of any C ABI. It was declared extern "C" here and linked anyway only because nothing referenced
 * od_session_app_device_id() until the dispatcher did: --gc-sections dropped the section and the
 * mismatch with it. */
void getAuthDeviceIdBytes(uint8_t* device_id);

struct od_session *od_session_app_state(void)
{
    return &g_session;
}

const struct SecurityConfig *od_session_app_security(void)
{
    return &securityConfig;
}

uint32_t od_session_app_now_ms(void)
{
    return od_hal_uptime_ms();
}

void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
    getAuthDeviceIdBytes(out);
}

/* Budgets for the nonce-rejection lines. Nonce failures deliberately do not count as integrity
 * strikes, so nothing else throttles a peer that drives them, and out-of-window fires routinely on
 * a lossy link. One budget PER REASON, not one shared: a stale client spamming session-id
 * mismatches must not be able to silence the out-of-window line, which is the one that reports
 * real transfer loss. */
static uint32_t s_logWindowMs = 0;   /* replay / out-of-window */
static uint32_t s_logOtherMs  = 0;   /* wrong session, bad tag, malformed, engine fault */

static bool budgetAllows(uint32_t* last_ms)
{
    const uint32_t now = od_hal_uptime_ms();
    if (*last_ms != 0 && (uint32_t)(now - *last_ms) < 5000u) return false;
    *last_ms = now;
    return true;
}

void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report)
{
    switch (op) {
    case OD_SESSION_APP_AUTH:
        switch ((enum od_session_auth)result) {
        case OD_SESSION_AUTH_CHALLENGE:
            od_log_info("Authentication challenge sent");
            break;
        case OD_SESSION_AUTH_ESTABLISHED:
            od_log_info("Authentication successful, session established");
            break;
        case OD_SESSION_AUTH_REJECTED:
            od_log_error("ERROR: Authentication failed (wrong key)");
            break;
        case OD_SESSION_AUTH_RATE_LIMITED:
            od_log_warn("Authentication rate limited (%u attempts in the window)",
                        (unsigned)(report ? report->attempts : 0u));
            break;
        case OD_SESSION_AUTH_NOT_CONFIGURED:
            od_log_error("ERROR: Authentication requested but encryption is not configured");
            break;
        case OD_SESSION_AUTH_EXPIRED:
            od_log_error("ERROR: Server nonce expired");
            break;
        case OD_SESSION_AUTH_CRYPTO_ERROR:
            od_log_error("ERROR: Crypto engine failure during authentication (status %d)",
                         (int)(report ? report->crypto_status : OD_HAL_CRYPTO_OK));
            break;
        default:
            od_log_error("ERROR: Invalid authentication request (rc=%d)", result);
            break;
        }
        break;

    case OD_SESSION_APP_OPEN: {
        if ((enum od_session_open)result == OD_SESSION_OPEN_OK) break;
        const uint8_t reason = report ? report->nonce_reason : 0u;
        const bool nonce_loss = (reason == (uint8_t)NONCE_OUT_OF_WINDOW ||
                                 reason == (uint8_t)NONCE_REPLAY);
        if (budgetAllows(nonce_loss ? &s_logWindowMs : &s_logOtherMs)) {
            od_log_error("ERROR: Decryption failed (0x%04X, rc=%d, nonce_reason=%u)",
                         (unsigned)cmd, result, (unsigned)reason);
        }
        break;
    }

    case OD_SESSION_APP_SEAL:
        if ((enum od_session_seal)result != OD_SESSION_SEAL_OK) {
            od_log_warn("Response seal failed (0x%04X, rc=%d)", (unsigned)cmd, result);
        }
        break;

    case OD_SESSION_APP_ALIVE:
    default:
        break;                        /* the liveness probe runs per frame; logging it is noise */
    }
}

/* od_txq's drop seam. A discarded response is invisible from the wire -- the host simply waits --
 * so this is the only place a permanently refusing transport, or a link that died with frames
 * still queued, leaves a trace. Deliberately not throttled: unlike the nonce lines above, a peer
 * cannot drive this at will, and losing one of these hides the cause of a client-side timeout. */
extern "C" void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    od_log_warn("TX dropped: origin=%u tag=%08lX len=%u reason=%d",
                (unsigned)(rp ? rp->origin : 0u), (unsigned long)(rp ? rp->tag : 0u),
                (unsigned)len, (int)why);
}
