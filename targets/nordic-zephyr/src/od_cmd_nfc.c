/* od_cmd_nfc.c -- CMD_NFC_ENDPOINT (0x0083) routing for this target.
 *
 * TEMPORARY. Dispatch still names od_cmd_app_nfc, so the shared machine is reached through this
 * wrapper until the opcode's dispatch row points at od_nfc_frame directly. Then this file goes.
 *
 * The parsing, the chunk assembler, the record-type and length rules and the 244-byte response
 * buffer that used to live here are shared/core/od_nfc.c's; the tag itself is reached through
 * od_nfc_app.h, implemented by opendisplay_nfc.c.
 */

#include "od_cmd_app.h"
#include "od_nfc.h"

od_cmd_result_t od_cmd_app_nfc(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return od_nfc_frame(ctx, body);
}
