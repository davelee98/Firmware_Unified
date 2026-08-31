/* od_rxq_app.c -- this target's answers to shared/core/od_rxq_app.h.
 *
 * The arrival and drop lines are no longer here: od_rxq.c logs them, one wording for both
 * targets. That is also what closed this target's coverage gap -- an empty or oversized frame
 * used to be reported as "pipe queue full", pointing at ring depth for a malformed frame.
 *
 * Both run on the BT RX THREAD and are only reached in a build compiled at OD_LOG_DEBUG.
 */

#include "od_rxq_app.h"

#include "od_xfer.h"
#include "opendisplay_config_parser.h"

bool od_rxq_app_encryption_enabled(void)
{
  return od_security_enabled_snapshot();
}

bool od_rxq_app_quiet(uint16_t cmd)
{
  return od_xfer_log_quiet(cmd);
}
