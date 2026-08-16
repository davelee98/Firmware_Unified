/* od_session_app.c -- this target's implementation of shared/core/od_session_app.h.
 *
 * The facts od_session deliberately did NOT promote, because each is genuinely per-target: which
 * session object, which security configuration, which clock, which silicon identity, and where
 * reports go. Everything else about the handshake, the KDF, the replay window and the CCM envelope
 * is shared.
 *
 * THE REPORT CALLBACK IS WHY shared/ CAN STAY SILENT. od_log.h is target-local, so od_session
 * cannot log; it hands out a result and a filled report, and this file decides the wording. That
 * is also what lets the shared code fire a report BEFORE the dispatcher takes its PIPE
 * silent-return arm -- the loss telemetry that an earlier Nordic port lost entirely.
 */

#include "od_session_app.h"

#include "opendisplay_config_parser.h"
#include "od_log.h"

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

/* THIS TARGET'S ONE SESSION, and it is file-static so that it is reachable only through the seam
 * below. An exported singleton is what lets a caller memset it -- which drops a live PSA key
 * handle from a finite pool without releasing it. od_session_clear() is the only teardown. */
static struct od_session s_session;

/* Nonce-rejection logs are rate-limited PER SITE at five seconds, not globally: a stale client
 * spamming session-id mismatches must not be able to silence the out-of-window line, which is the
 * one that reports real transfer loss on a lossy link. Once nonce failures stopped counting toward
 * integrity_failures, nothing else throttles a peer that drives these. */
static uint32_t s_log_window_ms;   /* replay / out-of-window */
static uint32_t s_log_other_ms;    /* wrong session, bad tag, malformed, engine fault */

static bool budget_allows(uint32_t *last_ms)
{
  const uint32_t now = k_uptime_get_32();

  if (*last_ms != 0u && (uint32_t)(now - *last_ms) < 5000u) {
    return false;
  }
  *last_ms = now;
  return true;
}

struct od_session *od_session_app_state(void)
{
  return &s_session;
}

const struct SecurityConfig *od_session_app_security(void)
{
  return od_get_parsed_security();
}

uint32_t od_session_app_now_ms(void)
{
  return k_uptime_get_32();
}

/* The low 32 bits of the 64-bit hwinfo id, big-endian. WIRE-VISIBLE: this packing feeds both the
 * KDF and the auth proof, so a different one is a different device to the host. */
void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
  /* ZEROED, and that is the whole of the fix. hwinfo's status is ignored -- a device that cannot
   * report an id still has to answer AUTHENTICATE with something -- but an uninitialised buffer
   * makes that something STACK RESIDUE, so the identity could differ between two boots of the
   * same board. A host keys its stored session key on this: a changing id is a device that
   * silently stops being the one that was provisioned. Zero is at least the same answer twice. */
  uint8_t id[8] = {0};
  uint64_t uid = 0;
  unsigned i;

  (void)hwinfo_get_device_id(id, sizeof(id));
  for (i = 0; i < sizeof(id); i++) {
    uid = (uid << 8) | id[i];
  }
  out[0] = (uint8_t)((uid >> 24) & 0xFFu);
  out[1] = (uint8_t)((uid >> 16) & 0xFFu);
  out[2] = (uint8_t)((uid >> 8) & 0xFFu);
  out[3] = (uint8_t)(uid & 0xFFu);
}

void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                          const struct od_session_report *report)
{
  switch (op) {
  case OD_SESSION_APP_AUTH:
    switch ((enum od_session_auth)result) {
    case OD_SESSION_AUTH_CHALLENGE:
      od_log_info("auth: challenge sent");
      break;
    case OD_SESSION_AUTH_ESTABLISHED:
      od_log_info("auth: session established");
      break;
    case OD_SESSION_AUTH_RATE_LIMITED:
      od_log_warn("auth: rate limited (%u attempts in window)",
                  (unsigned)(report ? report->attempts : 0u));
      break;
    case OD_SESSION_AUTH_CRYPTO_ERROR:
      od_log_error("auth: crypto engine failure (status %d)",
                   (int)(report ? report->crypto_status : OD_HAL_CRYPTO_OK));
      break;
    default:
      od_log_warn("auth: refused (rc=%d, status 0x%02X)", result,
                  (unsigned)(report ? report->status_byte : 0u));
      break;
    }
    break;

  case OD_SESSION_APP_OPEN: {
    uint8_t reason;
    bool nonce_loss;

    if ((enum od_session_open)result == OD_SESSION_OPEN_OK) {
      break;
    }
    reason = report ? report->nonce_reason : 0u;
    nonce_loss = (reason == (uint8_t)NONCE_OUT_OF_WINDOW || reason == (uint8_t)NONCE_REPLAY);
    if (budget_allows(nonce_loss ? &s_log_window_ms : &s_log_other_ms)) {
      od_log_error("decrypt failed (0x%04X, rc=%d, nonce_reason=%u)", (unsigned)cmd, result,
                   (unsigned)reason);
    }
    break;
  }

  case OD_SESSION_APP_SEAL:
    if ((enum od_session_seal)result != OD_SESSION_SEAL_OK) {
      od_log_warn("response seal failed (0x%04X, rc=%d)", (unsigned)cmd, result);
    }
    break;

  case OD_SESSION_APP_ALIVE:
  default:
    break;                          /* the liveness probe runs per frame; logging it is noise */
  }
}
