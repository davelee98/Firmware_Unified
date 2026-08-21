/* Host-only constructor for command contexts passed directly to production handlers. */
#ifndef OD_CMD_TEST_CTX_H
#define OD_CMD_TEST_CTX_H

#include "od_cmd.h"

static inline od_cmd_ctx_t od_test_cmd_ctx(od_reply_t rp, od_tx_reservation_t *r,
                                            uint16_t wire_len, bool was_protected)
{
    od_cmd_ctx_t ctx;

    ctx.rp = rp;
    ctx.r = r;
    ctx.wire_len = wire_len;
    ctx.was_protected = was_protected;
    return ctx;
}

#endif /* OD_CMD_TEST_CTX_H */
