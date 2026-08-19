#include "od_xfer_internal.h"

#include "opendisplay_protocol.h"

#include <limits.h>
#include <string.h>

#define OD_PARTIAL_FLAG_COMPRESSED 0x01u
#define OD_PARTIAL_ALLOWED_FLAGS   OD_PARTIAL_FLAG_COMPRESSED
#define OD_REFRESH_FULL            0u
#define OD_REFRESH_FAST            1u
#define OD_REFRESH_PARTIAL         2u

#if OD_CAP_PARTIAL
static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static od_cmd_result_t partial_fail(const od_cmd_ctx_t *ctx, uint8_t opcode, uint8_t error,
                                    bool abort_active)
{
    od_xfer_reply_partial_error(ctx, opcode, error, abort_active);
    return OD_CMD_NACK;
}

od_cmd_result_t od_xfer_partial_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_panel_info_t panel;
    od_color_geometry_t rect_geometry;
    od_xfer_state_t *state = od_xfer_state();
    uint8_t flags;
    uint32_t old_etag;
    uint32_t new_etag;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t plane_bytes;
    uint32_t expected;
    const uint8_t ack[] = { RESP_ACK, 0x76u };

    if (ctx == NULL || !od_span_valid(body) || body.n > UINT32_MAX) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_STREAM, false);
    }
    od_xfer_replace_active();
    if (body.n < 17u) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_STREAM, false);
    }

    flags = body.p[0];
    old_etag = read_be32(body.p + 1u);
    new_etag = read_be32(body.p + 5u);
    x = read_be16(body.p + 9u);
    y = read_be16(body.p + 11u);
    width = read_be16(body.p + 13u);
    height = read_be16(body.p + 15u);

    if ((flags & (uint8_t)~OD_PARTIAL_ALLOWED_FLAGS) != 0u) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_FLAGS, false);
    }
    if (old_etag == 0u || old_etag != od_xfer_app_displayed_etag() || new_etag == 0u) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_ETAG_MISMATCH, false);
    }
    memset(&panel, 0, sizeof panel);
    if (!od_xfer_app_panel_info(&panel) || !panel.partial_enabled
        || !panel.geometry.partial_supported) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_UNSUPPORTED, false);
    }
    if (width == 0u || height == 0u
        || (uint32_t)x + width > panel.width
        || (uint32_t)y + height > panel.height) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_RECT_OOB, false);
    }
    if ((x & 7u) != 0u || (width & 7u) != 0u) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_RECT_ALIGN, false);
    }
    if (od_color_direct_geometry(OD_COLOR_SCHEME_MONO, width, height, &rect_geometry)
            != OD_COLOR_OK
        || rect_geometry.part_bytes[0] == 0u
        || rect_geometry.part_bytes[0] > UINT32_MAX / 2u) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_STREAM, false);
    }
    plane_bytes = rect_geometry.part_bytes[0];
    expected = plane_bytes * 2u;

    od_xfer_clear_state();
    state->mode = OD_XFER_DIRECT_PARTIAL;
    state->owner = ctx->rp;
    state->started_ms = od_xfer_app_now_ms();
    state->expected_bytes = expected;
    state->compressed = (flags & OD_PARTIAL_FLAG_COMPRESSED) != 0u;
    state->geometry = panel.geometry;
    state->partial.new_etag = new_etag;
    state->partial.plane_bytes = plane_bytes;
    state->partial.x = x;
    state->partial.y = y;
    state->partial.width = width;
    state->partial.height = height;

    if (!od_xfer_app_begin_partial(x, y, width, height, plane_bytes)) {
        return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_STREAM, true);
    }
    if (state->compressed) {
        od_xfer_stream_reset(expected);
    }
    if (body.n > 17u) {
        od_span_t inline_input = od_span_drop(body, 17u);
        state->received_bytes = (uint32_t)inline_input.n;
        if (state->compressed) {
            if (!od_xfer_stream_push(inline_input, false)) {
                return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_STREAM, true);
            }
        } else {
            uint32_t consumed;
            if (inline_input.n > expected) {
                return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_STREAM, true);
            }
            consumed = od_xfer_app_write(0u, inline_input);
            if (consumed != inline_input.n) {
                return partial_fail(ctx, 0x76u, OD_ERR_PARTIAL_STREAM, true);
            }
            state->written_bytes = consumed;
        }
    }
    if (od_xfer_reply_app(ctx, ack, (uint16_t)sizeof ack) != OD_TXQ_OK) {
        od_xfer_abort_active(true);
        return OD_CMD_NACK;
    }
    return OD_CMD_OK;
}

od_cmd_result_t od_xfer_partial_data_impl(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_state_t *state = od_xfer_state();
    const uint8_t ack[] = { RESP_ACK, RESP_DIRECT_WRITE_DATA_ACK };

    if (!od_xfer_owner_matches(ctx) || body.n == 0u) {
        return OD_CMD_OK;
    }
    if (body.n > UINT32_MAX) {
        return partial_fail(ctx, RESP_DIRECT_WRITE_DATA_ACK, OD_ERR_PARTIAL_STREAM, true);
    }
    if (state->compressed) {
        if (body.n > UINT32_MAX - state->received_bytes) {
            return partial_fail(ctx, RESP_DIRECT_WRITE_DATA_ACK, OD_ERR_PARTIAL_STREAM, true);
        }
        state->received_bytes += (uint32_t)body.n;
        if (!od_xfer_stream_push(body, false)) {
            return partial_fail(ctx, RESP_DIRECT_WRITE_DATA_ACK, OD_ERR_PARTIAL_STREAM, true);
        }
    } else {
        uint32_t consumed;
        if (state->written_bytes > state->expected_bytes
            || body.n > (size_t)(state->expected_bytes - state->written_bytes)) {
            return partial_fail(ctx, RESP_DIRECT_WRITE_DATA_ACK, OD_ERR_PARTIAL_STREAM, true);
        }
        consumed = od_xfer_app_write(state->written_bytes, body);
        if (consumed != (uint32_t)body.n) {
            return partial_fail(ctx, RESP_DIRECT_WRITE_DATA_ACK, OD_ERR_PARTIAL_STREAM, true);
        }
        state->written_bytes += consumed;
    }
    if (od_xfer_reply_app(ctx, ack, (uint16_t)sizeof ack) != OD_TXQ_OK) {
        od_xfer_abort_active(true);
        return OD_CMD_NACK;
    }
    return OD_CMD_OK;
}

od_cmd_result_t od_xfer_partial_end_impl(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_state_t *state = od_xfer_state();
    const uint8_t ack[] = { RESP_ACK, RESP_DIRECT_WRITE_END_ACK };
    const uint8_t success[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_SUCCESS };
    const uint8_t timeout[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_TIMEOUT };
    uint8_t refresh_mode = OD_REFRESH_PARTIAL;
    bool completed = false;

    if (!od_xfer_owner_matches(ctx)) {
        return OD_CMD_OK;
    }
    if (body.n > 1u) {
        return partial_fail(ctx, RESP_DIRECT_WRITE_END_ACK, OD_ERR_PARTIAL_STREAM, true);
    }
    if (state->compressed) {
        if (state->received_bytes == 0u || !od_xfer_stream_push(od_span_none(), true)) {
            return partial_fail(ctx, RESP_DIRECT_WRITE_END_ACK, OD_ERR_PARTIAL_STREAM, true);
        }
    } else if (state->written_bytes != state->expected_bytes) {
        return partial_fail(ctx, RESP_DIRECT_WRITE_END_ACK, OD_ERR_PARTIAL_STREAM, true);
    }

    if (od_xfer_reply_app(ctx, ack, (uint16_t)sizeof ack) != OD_TXQ_OK) {
        od_xfer_app_set_displayed_etag(0u);
        od_xfer_app_barrier_abort(&state->owner);
        od_xfer_clear_state();
        return OD_CMD_NACK;
    }
    if (od_xfer_app_before_refresh(&state->owner) != OD_XFER_BARRIER_PROCEED) {
        od_xfer_app_set_displayed_etag(0u);
        od_xfer_app_barrier_abort(&state->owner);
        od_xfer_clear_state();
        return OD_CMD_NACK;
    }
    if (body.n == 1u && body.p[0] == OD_REFRESH_FULL) {
        refresh_mode = OD_REFRESH_FULL;
    } else if (body.n == 1u && body.p[0] == OD_REFRESH_FAST) {
        refresh_mode = OD_REFRESH_FAST;
    }
    if (!od_xfer_app_refresh(refresh_mode, &completed)) {
        return partial_fail(ctx, RESP_DIRECT_WRITE_END_ACK, OD_ERR_PARTIAL_STREAM, true);
    }
    od_xfer_app_set_displayed_etag(completed ? state->partial.new_etag : 0u);
    od_xfer_clear_state();
    if (od_xfer_reply_app(ctx, completed ? success : timeout,
                          (uint16_t)(completed ? sizeof success : sizeof timeout)) != OD_TXQ_OK) {
        return OD_CMD_NACK;
    }
    return OD_CMD_OK;
}

#else

od_cmd_result_t od_xfer_partial_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const uint8_t frame[] = { RESP_NACK, 0x76u, OD_ERR_PARTIAL_UNSUPPORTED, 0u };
    (void)body;
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
    return OD_CMD_NACK;
}

#endif
