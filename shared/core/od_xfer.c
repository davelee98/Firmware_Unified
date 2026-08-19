#include "od_xfer_internal.h"

#include "od_reply.h"
#include "opendisplay_protocol.h"

#include <limits.h>
#include <string.h>

static od_xfer_state_t s_xfer;

od_xfer_state_t *od_xfer_state(void)
{
    return &s_xfer;
}

bool od_xfer_active(void)
{
    return s_xfer.mode != OD_XFER_IDLE;
}

bool od_xfer_owner_matches(const od_cmd_ctx_t *ctx)
{
    return ctx != NULL && ctx->rp.origin == s_xfer.owner.origin && ctx->rp.tag == s_xfer.owner.tag;
}

void od_xfer_clear_state(void)
{
    memset(&s_xfer, 0, sizeof s_xfer);
    s_xfer.mode = OD_XFER_IDLE;
}

void od_xfer_replace_active(void)
{
    if (od_xfer_active()) {
        od_xfer_app_abort();
        od_xfer_clear_state();
    }
}

void od_xfer_abort_active(bool clear_etag)
{
    if (od_xfer_active()) {
        od_xfer_app_abort();
    }
#if OD_CAP_PARTIAL
    if (clear_etag) {
        od_xfer_app_set_displayed_etag(0u);
    }
#else
    (void)clear_etag;
#endif
    od_xfer_clear_state();
}

void od_xfer_reset(void)
{
    od_xfer_abort_active(false);
}

od_txq_status_t od_xfer_reply_app(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    if (ctx == NULL) {
        return OD_TXQ_INVARIANT;
    }
    return od_reply(ctx->r, &ctx->rp, frame, len);
}

void od_xfer_reply_error(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    if (ctx != NULL) {
        (void)od_reply_plain(ctx->r, &ctx->rp, frame, len);
    }
}

void od_xfer_reply_simple_error(const od_cmd_ctx_t *ctx, uint8_t opcode)
{
    const uint8_t frame[] = { RESP_NACK, opcode };
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
}

#if OD_CAP_PARTIAL
void od_xfer_reply_partial_error(const od_cmd_ctx_t *ctx, uint8_t opcode, uint8_t error,
                                 bool abort_active)
{
    const uint8_t frame[] = { RESP_NACK, opcode, error, 0u };

    od_xfer_app_set_displayed_etag(0u);
    if (abort_active) {
        od_xfer_abort_active(false);
    }
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
}
#endif

static bool stream_sink(void *ctx, od_mut_span_t bytes)
{
    od_xfer_state_t *state = (od_xfer_state_t *)ctx;
    uint32_t consumed;

    if (state == NULL || bytes.n == 0u || bytes.n > UINT32_MAX) {
        return false;
    }
    if (state->written_bytes > state->expected_bytes
        || bytes.n > (size_t)(state->expected_bytes - state->written_bytes)) {
        return false;
    }
    consumed = od_xfer_app_write(state->written_bytes, od_mut_span_const(bytes));
    if (consumed != (uint32_t)bytes.n) {
        return false;
    }
    state->written_bytes += consumed;
    return true;
}

void od_xfer_stream_reset(uint32_t expected_bytes)
{
    od_zlib_pump_reset(expected_bytes);
}

bool od_xfer_stream_push(od_span_t input, bool final)
{
    od_mut_span_t scratch = od_xfer_app_inflate_scratch();
    od_zlib_pump_status_t status;

    if (!od_span_valid(input) || !od_mut_span_valid(scratch) || scratch.n == 0u) {
        return false;
    }
    status = od_zlib_pump_push(input, final, scratch, stream_sink, &s_xfer);
    if (status == OD_ZLIB_PUMP_ERROR) {
        return false;
    }
    return !final || status == OD_ZLIB_PUMP_DONE;
}

od_cmd_result_t od_xfer_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (!od_span_valid(body)) {
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_DATA_ACK);
        return OD_CMD_NACK;
    }
    switch (s_xfer.mode) {
    case OD_XFER_DIRECT_FULL:
        return od_xfer_direct_data_impl(ctx, body);
#if OD_CAP_PARTIAL
    case OD_XFER_DIRECT_PARTIAL:
        return od_xfer_partial_data_impl(ctx, body);
#else
    case OD_XFER_DIRECT_PARTIAL:
        return OD_CMD_OK;
#endif
    case OD_XFER_IDLE:
    case OD_XFER_PIPE_FULL:
    case OD_XFER_PIPE_PARTIAL:
    case OD_XFER_FATAL:
        return OD_CMD_OK;
    }
    return OD_CMD_NACK;
}

od_cmd_result_t od_xfer_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (!od_span_valid(body)) {
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_END_ACK);
        return OD_CMD_NACK;
    }
    switch (s_xfer.mode) {
    case OD_XFER_DIRECT_FULL:
        return od_xfer_direct_end_impl(ctx, body);
#if OD_CAP_PARTIAL
    case OD_XFER_DIRECT_PARTIAL:
        return od_xfer_partial_end_impl(ctx, body);
#else
    case OD_XFER_DIRECT_PARTIAL:
        return OD_CMD_OK;
#endif
    case OD_XFER_IDLE:
    case OD_XFER_PIPE_FULL:
    case OD_XFER_PIPE_PARTIAL:
    case OD_XFER_FATAL:
        return OD_CMD_OK;
    }
    return OD_CMD_NACK;
}
