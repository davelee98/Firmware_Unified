/* od_cmd_reply.c -- see od_cmd_reply.h. */

#include "od_cmd_reply.h"

#include "od_reply.h"

#include <stddef.h>

od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (ctx == NULL || frame == NULL || len == 0u) {
    return OD_TXQ_INVARIANT;
  }
  return od_reply(ctx->r, &ctx->rp, frame, len);
}

od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (ctx == NULL || frame == NULL || len == 0u) {
    return OD_TXQ_INVARIANT;
  }
  return od_reply_plain(ctx->r, &ctx->rp, frame, len);
}
