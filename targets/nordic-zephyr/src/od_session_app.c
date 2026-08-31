/* od_session_app.c -- this target's implementation of shared/core/od_session_app.h.
 *
 * The facts od_session deliberately did NOT promote, because each is genuinely per-target: which
 * session object, which security configuration, which clock, which silicon identity, and where
 * reports go. Everything else about the handshake, the KDF, the replay window and the CCM envelope
 * is shared.
 *
 * The report callback is no longer where the wording lives. od_log.h is shared/core and pure, so
 * od_session.c logs each outcome itself, at the point it decides it. This file keeps the four
 * accessors and an empty implementation of the callback -- see the note above it.
 */

#include "od_session_app.h"

#include "opendisplay_config_parser.h"

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

/* THIS TARGET'S ONE SESSION, and it is file-static so that it is reachable only through the seam
 * below. An exported singleton is what lets a caller memset it -- which drops a live PSA key
 * handle from a finite pool without releasing it. od_session_clear() is the only teardown. */
static struct od_session s_session;

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

/* EMPTY ON PURPOSE. shared/core/od_session.c now logs every auth, decrypt and seal outcome at
 * the point it decides it, so one wording serves both targets -- which is how this target came
 * to be silent on OD_SESSION_AUTH_NOT_CONFIGURED and OD_SESSION_AUTH_EXPIRED, both of which fell
 * into the default arm here and were reported as a generic refusal.
 *
 * The callback survives because od_gate.c and od_reply.c still call it and BG22 implements it
 * with printf, which is that target's only auth and decrypt diagnostic. */
void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                          const struct od_session_report *report)
{
  (void)op;
  (void)result;
  (void)cmd;
  (void)report;
}
