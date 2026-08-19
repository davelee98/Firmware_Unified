/* od_cmd_direct.c -- temporary dispatch bridges to shared direct/partial policy.
 *
 * The opcode table still names the target hook surface while targets are cut over independently;
 * these wrappers must contain no transfer policy of their own.
 */

#include "od_cmd_app.h"

#include "od_xfer.h"

od_cmd_result_t od_cmd_app_partial_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return od_xfer_partial_start(ctx, body);
}

od_cmd_result_t od_cmd_app_direct_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return od_xfer_direct_start(ctx, body);
}

od_cmd_result_t od_cmd_app_direct_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return od_xfer_data(ctx, body);
}

od_cmd_result_t od_cmd_app_direct_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return od_xfer_end(ctx, body);
}
