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

bool od_xfer_owns_hardware(void)
{
    return s_xfer.mode != OD_XFER_IDLE && s_xfer.mode != OD_XFER_FATAL;
}

bool od_xfer_frames_may_arrive(void)
{
    return s_xfer.mode != OD_XFER_IDLE;
}

bool od_xfer_owner(od_reply_t *out)
{
    if (!od_xfer_active() || out == NULL) {
        return false;
    }
    *out = s_xfer.owner;
    return true;
}

bool od_xfer_started_ms(uint32_t *out)
{
    if (!od_xfer_active() || out == NULL) {
        return false;
    }
    *out = s_xfer.started_ms;
    return true;
}

bool od_xfer_owner_matches(const od_cmd_ctx_t *ctx)
{
    return ctx != NULL && od_reply_same(&ctx->rp, &s_xfer.owner);
}

void od_xfer_clear_state(void)
{
    memset(&s_xfer, 0, sizeof s_xfer);
    s_xfer.mode = OD_XFER_IDLE;
    od_pipe_reset_state();
}

void od_xfer_replace_active(void)
{
    if (od_xfer_active()) {
        if (od_xfer_owns_hardware()) {
            od_xfer_app_abort(OD_XFER_ABORT_REPLACED);
        }
        od_xfer_clear_state();
    }
}

void od_xfer_abort_active(od_xfer_abort_reason_t reason, bool clear_etag)
{
    if (od_xfer_owns_hardware()) {
        od_xfer_app_abort(reason);
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
    od_xfer_abort_active(OD_XFER_ABORT_RESET, false);
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
                                 bool abort_active, od_xfer_abort_reason_t reason)
{
    const uint8_t frame[] = { RESP_NACK, opcode, error, 0u };

    od_xfer_app_set_displayed_etag(0u);
    if (abort_active) {
        od_xfer_abort_active(reason, false);
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

bool od_xfer_pipe_arm_full(const od_cmd_ctx_t *ctx, uint32_t total, bool compressed)
{
    od_xfer_panel_info_t panel;

    if (ctx == NULL || total == 0u || s_xfer.mode != OD_XFER_IDLE) {
        return false;
    }
    od_xfer_app_prepare_start();
    memset(&panel, 0, sizeof panel);
    if (!od_xfer_app_panel_info(&panel)
        || panel.geometry.total_bytes == 0u
        || panel.geometry.layout == OD_COLOR_LAYOUT_SPLIT_HALVES
        || total != panel.geometry.total_bytes) {
        return false;
    }

    s_xfer.mode = OD_XFER_PIPE_FULL;
    s_xfer.owner = ctx->rp;
    s_xfer.started_ms = od_xfer_app_now_ms();
    s_xfer.expected_bytes = total;
    s_xfer.compressed = compressed;
    s_xfer.geometry = panel.geometry;
    if (compressed) {
        od_xfer_stream_reset(total);
    }
    return true;
}

#if OD_CAP_PARTIAL
bool od_xfer_pipe_arm_partial(const od_cmd_ctx_t *ctx, uint32_t total, bool compressed,
                              uint32_t old_etag, uint16_t x, uint16_t y,
                              uint16_t width, uint16_t height, uint8_t *err_out)
{
    od_xfer_panel_info_t panel;
    od_color_geometry_t rect;
    uint32_t plane_bytes;

    if (err_out != NULL) {
        *err_out = OD_ERR_PIPE_START_BAD_HEADER;
    }
    if (ctx == NULL || total == 0u || s_xfer.mode != OD_XFER_IDLE) {
        return false;
    }
    od_xfer_app_prepare_start();
    memset(&panel, 0, sizeof panel);
    if (!od_xfer_app_panel_info(&panel) || !panel.partial_enabled
        || !panel.geometry.partial_supported) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_PARTIAL_UNSUPPORTED;
        }
        return false;
    }
    if (old_etag == 0u || old_etag != od_xfer_app_displayed_etag()) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_ETAG_MISMATCH;
        }
        return false;
    }
    if (width == 0u || height == 0u
        || (uint32_t)x + width > panel.width
        || (uint32_t)y + height > panel.height
        || (x & 7u) != 0u || (width & 7u) != 0u) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_RECT_INVALID;
        }
        return false;
    }
    if (od_color_direct_geometry(OD_COLOR_SCHEME_MONO, width, height, &rect) != OD_COLOR_OK
        || rect.part_bytes[0] == 0u || rect.part_bytes[0] > UINT32_MAX / 2u) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_RECT_INVALID;
        }
        return false;
    }
    plane_bytes = rect.part_bytes[0];
    if (total != plane_bytes * 2u) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_SIZE_MISMATCH;
        }
        return false;
    }

    s_xfer.mode = OD_XFER_PIPE_PARTIAL;
    s_xfer.owner = ctx->rp;
    s_xfer.started_ms = od_xfer_app_now_ms();
    s_xfer.expected_bytes = total;
    s_xfer.compressed = compressed;
    s_xfer.geometry = panel.geometry;
    s_xfer.partial.plane_bytes = plane_bytes;
    s_xfer.partial.x = x;
    s_xfer.partial.y = y;
    s_xfer.partial.width = width;
    s_xfer.partial.height = height;
    if (compressed) {
        od_xfer_stream_reset(total);
    }
    return true;
}
#endif

bool od_xfer_pipe_activate(void)
{
    bool ok = false;

    switch (s_xfer.mode) {
    case OD_XFER_PIPE_FULL:
        ok = od_xfer_app_begin_full(&s_xfer.geometry);
        break;
#if OD_CAP_PARTIAL
    case OD_XFER_PIPE_PARTIAL:
        ok = od_xfer_app_begin_partial(s_xfer.partial.x, s_xfer.partial.y,
                                       s_xfer.partial.width, s_xfer.partial.height,
                                       s_xfer.partial.plane_bytes);
        break;
#else
    case OD_XFER_PIPE_PARTIAL:
        break;
#endif
    case OD_XFER_IDLE:
    case OD_XFER_DIRECT_FULL:
    case OD_XFER_DIRECT_PARTIAL:
    case OD_XFER_FATAL:
        break;
    }
    if (!ok) {
        od_xfer_pipe_enter_fatal();
    }
    return ok;
}

bool od_xfer_pipe_consume(od_span_t payload)
{
    uint32_t remaining;
    uint32_t consumed;

    if (!od_span_valid(payload) || payload.n == 0u || payload.n > UINT32_MAX
        || (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL)
        || payload.n > UINT32_MAX - s_xfer.received_bytes) {
        return false;
    }
    s_xfer.received_bytes += (uint32_t)payload.n;
    if (s_xfer.compressed) {
        return od_xfer_stream_push(payload, false);
    }
    if (s_xfer.written_bytes > s_xfer.expected_bytes) {
        return false;
    }
    remaining = s_xfer.expected_bytes - s_xfer.written_bytes;
    if (s_xfer.mode == OD_XFER_PIPE_PARTIAL && payload.n > remaining) {
        return false;
    }
    payload = od_span_take(payload, payload.n < remaining ? payload.n : remaining);
    if (payload.n == 0u) {
        return true;
    }
    consumed = od_xfer_app_write(s_xfer.written_bytes, payload);
    if (consumed != (uint32_t)payload.n) {
        return false;
    }
    s_xfer.written_bytes += consumed;
    return true;
}

bool od_xfer_pipe_finalize(void)
{
    if (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL) {
        return false;
    }
    if (!s_xfer.compressed) {
        return true;
    }
    return s_xfer.received_bytes != 0u && od_xfer_stream_push(od_span_none(), true);
}

bool od_xfer_pipe_complete(void)
{
    return (s_xfer.mode == OD_XFER_PIPE_FULL || s_xfer.mode == OD_XFER_PIPE_PARTIAL)
        && s_xfer.written_bytes == s_xfer.expected_bytes;
}

void od_xfer_pipe_enter_fatal(void)
{
    const bool partial = s_xfer.mode == OD_XFER_PIPE_PARTIAL;

    if (od_xfer_owns_hardware()) {
        od_xfer_app_abort(OD_XFER_ABORT_STREAM_FAILED);
    }
#if OD_CAP_PARTIAL
    if (partial) {
        od_xfer_app_set_displayed_etag(0u);
    }
#else
    (void)partial;
#endif
    if (s_xfer.mode != OD_XFER_IDLE) {
        s_xfer.mode = OD_XFER_FATAL;
    }
}

od_xfer_barrier_t od_xfer_pipe_before_refresh(void)
{
    if (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL) {
        return OD_XFER_BARRIER_ABORT;
    }
    return od_xfer_app_before_refresh(&s_xfer.owner);
}

void od_xfer_pipe_barrier_abort(void)
{
    const od_reply_t owner = s_xfer.owner;
#if OD_CAP_PARTIAL
    if (s_xfer.mode == OD_XFER_PIPE_PARTIAL) {
        od_xfer_app_set_displayed_etag(0u);
    }
#endif
    od_xfer_clear_state();
    od_xfer_app_barrier_abort(&owner);
}

bool od_xfer_pipe_refresh(uint8_t mode, bool has_new_etag, uint32_t new_etag,
                          bool *completed)
{
    const bool partial = s_xfer.mode == OD_XFER_PIPE_PARTIAL;
    bool refreshed;

    if (completed == NULL
        || (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL)) {
        return false;
    }
    refreshed = od_xfer_app_refresh(mode, completed);
    if (!refreshed) {
        od_xfer_abort_active(OD_XFER_ABORT_REFRESH_FAILED, partial || has_new_etag);
        return false;
    }
#if OD_CAP_PARTIAL
    if (partial) {
        od_xfer_app_set_displayed_etag(*completed && has_new_etag && new_etag != 0u
                                      ? new_etag : 0u);
    } else if (*completed) {
        od_xfer_app_set_displayed_etag(has_new_etag && new_etag != 0u ? new_etag : 0u);
    } else if (has_new_etag) {
        od_xfer_app_set_displayed_etag(0u);
    }
#else
    (void)partial;
    (void)has_new_etag;
    (void)new_etag;
#endif
    od_xfer_clear_state();
    return true;
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
