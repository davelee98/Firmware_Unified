/* od_cmd_reply.cpp -- see od_cmd_reply.h. Legacy routing; the cutover flips OD_CMD_REPLY_SHARED. */

#include "od_cmd_reply.h"

#include "od_reply.h"

/* 0 until the cutover: replies go to the shipped sender and nothing observable changes while the
 * 78 call sites are converted. 1 routes them through od_txq, which is the whole point and is
 * ALSO the moment the queue must actually be drained -- see the handler-rewrite plan's step 8,
 * which enables this flag and wires the pump in one commit. Flipping it alone produces a device
 * that accepts commands and answers none. */
#ifndef OD_CMD_REPLY_SHARED
#define OD_CMD_REPLY_SHARED 0
#endif

extern "C" void sendResponse(uint8_t* response, uint16_t len);
extern "C" void sendResponseUnencrypted(uint8_t* response, uint16_t len);

extern "C" od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    if (ctx == nullptr || frame == nullptr || len == 0u) {
        return OD_TXQ_INVARIANT;
    }
#if OD_CMD_REPLY_SHARED
    return od_reply(ctx->r, &ctx->rp, frame, len);
#else
    /* The shipped sender infers plain-vs-sealed from the bytes. Routing here preserves that
     * exactly, so this step is observably a no-op -- the CLASSIFICATION is what the call site has
     * gained, and it only takes effect at the cutover. */
    sendResponse(const_cast<uint8_t *>(frame), len);
    return OD_TXQ_OK;
#endif
}

extern "C" od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx,
                                              const uint8_t *frame, uint16_t len)
{
    if (ctx == nullptr || frame == nullptr || len == 0u) {
        return OD_TXQ_INVARIANT;
    }
#if OD_CMD_REPLY_SHARED
    return od_reply_plain(ctx->r, &ctx->rp, frame, len);
#else
    sendResponseUnencrypted(const_cast<uint8_t *>(frame), len);
    return OD_TXQ_OK;
#endif
}
