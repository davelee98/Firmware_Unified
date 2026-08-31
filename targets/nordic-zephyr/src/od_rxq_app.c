/* od_rxq_app.c -- this target's answers to shared/core/od_rxq_app.h.
 *
 * The arrival and drop lines are no longer here: od_rxq.c logs them, one wording for both
 * targets. That is also what closed this target's coverage gap -- an empty or oversized frame
 * used to be reported as "pipe queue full", pointing at ring depth for a malformed frame.
 *
 * Both run on the BT RX THREAD and are only reached in a build compiled at OD_LOG_DEBUG.
 */

#include "od_rxq_app.h"

#include "opendisplay_config_parser.h"
#include "opendisplay_protocol.h"

bool od_rxq_app_encryption_enabled(void)
{
  return od_security_enabled_snapshot();
}

bool od_rxq_app_quiet(uint16_t cmd)
{
  /* Unconditional for these two opcodes, where ESP32 also requires a stream to be mid-flight.
   * This target has no chunk counter to consult yet; it gains one when the transfer logging
   * converges into od_xfer.c, at which point this becomes the same state-aware test. */
  return cmd == CMD_DIRECT_WRITE_DATA || cmd == CMD_PIPE_WRITE_DATA;
}
