/* od_cmd_reply.c -- see od_cmd_reply.h. Legacy routing; the cutover flips OD_CMD_REPLY_SHARED. */

#include "od_cmd_reply.h"

#include "od_reply.h"
#include "opendisplay_pipe_internal.h"

/* 0 until the cutover: replies go to the shipped sender and nothing observable changes while the
 * 70 call sites are converted. 1 routes them through od_txq, which is the whole point and is ALSO
 * the moment the queue must actually be drained. Flipping it alone produces a device that accepts
 * commands and answers none. */
#ifndef OD_CMD_REPLY_SHARED
#define OD_CMD_REPLY_SHARED 0
#endif

od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (ctx == NULL || frame == NULL || len == 0u) {
    return OD_TXQ_INVARIANT;
  }
#if OD_CMD_REPLY_SHARED
  return od_reply(ctx->r, &ctx->rp, frame, len);
#else
  /* The shipped sender infers plain-vs-sealed from the bytes. Routing here preserves that exactly,
   * so this step is observably a no-op -- the CLASSIFICATION is what the call site has gained, and
   * it only takes effect at the cutover. */
  od_pipe_legacy_send(frame, len);
  return OD_TXQ_OK;
#endif
}

od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (ctx == NULL || frame == NULL || len == 0u) {
    return OD_TXQ_INVARIANT;
  }
#if OD_CMD_REPLY_SHARED
  return od_reply_plain(ctx->r, &ctx->rp, frame, len);
#else
  /* ALSO the inferring sender, NOT a forced-plain one. In legacy mode both adapters must be
   * observably identical to what shipped, and pipe_send()'s own byte-2 inference already routes
   * the control frames plain. Forcing plain here would take effect IMMEDIATELY rather than at the
   * cutover, changing the wire for every site currently sealed by inference but classified plain.
   * The classification is recorded at the call site and takes effect when the flag flips. */
  od_pipe_legacy_send(frame, len);
  return OD_TXQ_OK;
#endif
}
