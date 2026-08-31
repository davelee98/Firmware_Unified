#include "od_xfer_internal.h"

#include "od_log.h"
#include "od_reply.h"
#include "opendisplay_protocol.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static od_xfer_state_t s_xfer;
static volatile uint8_t s_log_quiet;

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
static struct {
    uint32_t chunks;
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    uint16_t last_len;
    uint8_t last_step;
    uint8_t last_head_len;
    uint8_t last_head[16];
#endif
} s_xfer_log;
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
static void log_hex(char *buf, size_t size, const uint8_t *data, uint8_t len)
{
    size_t pos = 0u;
    uint8_t i;

    if (size == 0u) {
        return;
    }
    buf[0] = '\0';
    for (i = 0u; i < len && pos < size; ++i) {
        const int written = snprintf(buf + pos, size - pos, i == 0u ? "%02X" : " %02X",
                                     data[i]);
        if (written < 0 || (size_t)written >= size - pos) {
            break;
        }
        pos += (size_t)written;
    }
}
#endif

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

bool od_xfer_log_quiet(uint16_t opcode)
{
    if (opcode != CMD_DIRECT_WRITE_DATA && opcode != CMD_PIPE_WRITE_DATA) {
        return false;
    }
    return __atomic_load_n(&s_log_quiet, __ATOMIC_ACQUIRE) != 0u;
}

bool od_xfer_owner_matches(const od_cmd_ctx_t *ctx)
{
    return ctx != NULL && od_reply_same(&ctx->rp, &s_xfer.owner);
}

void od_xfer_clear_state(void)
{
    memset(&s_xfer, 0, sizeof s_xfer);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    memset(&s_xfer_log, 0, sizeof s_xfer_log);
#endif
    s_xfer.mode = OD_XFER_IDLE;
    __atomic_store_n(&s_log_quiet, 0u, __ATOMIC_RELEASE);
    od_pipe_reset_state();
}

void od_xfer_log_start(void)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    od_log_debug("DW start: %u bytes expected, %s", (unsigned)s_xfer.expected_bytes,
                 s_xfer.compressed ? "zlib streaming" : "raw (uncompressed)");
#endif
}

void od_xfer_log_chunk(od_span_t payload)
{
    if (!od_span_valid(payload) || payload.n == 0u) {
        return;
    }
    __atomic_store_n(&s_log_quiet, 1u, __ATOMIC_RELEASE);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    s_xfer_log.chunks++;
#endif
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    s_xfer_log.last_len = payload.n > UINT16_MAX ? UINT16_MAX : (uint16_t)payload.n;
    s_xfer_log.last_head_len = payload.n < sizeof s_xfer_log.last_head
        ? (uint8_t)payload.n : (uint8_t)sizeof s_xfer_log.last_head;
    memcpy(s_xfer_log.last_head, payload.p, s_xfer_log.last_head_len);
    if (s_xfer_log.chunks == 1u) {
        char hex[64];
        uint32_t estimate;

        log_hex(hex, sizeof hex, s_xfer_log.last_head, s_xfer_log.last_head_len);
        od_log_debug("DW frame 1: %u bytes: %s", (unsigned)payload.n, hex);
        if (s_xfer.expected_bytes != 0u) {
            estimate = ((s_xfer.expected_bytes - 1u) / (uint32_t)payload.n) + 1u;
            od_log_debug("DW expecting ~%u chunks", (unsigned)estimate);
        }
    }
#else
    (void)payload;
#endif
}

void od_xfer_log_progress(void)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    uint32_t percent;
    uint8_t step;

    if (s_xfer.expected_bytes == 0u) {
        return;
    }
    percent = (uint32_t)(((uint64_t)s_xfer.written_bytes * 100u) / s_xfer.expected_bytes);
    if (percent >= 100u) {
        return;
    }
    step = (uint8_t)(percent / 5u);
    if (step <= s_xfer_log.last_step) {
        return;
    }
    s_xfer_log.last_step = step;
    od_log_debug("DW %u%% (%u chunks, %u/%u bytes)", (unsigned)percent,
                 (unsigned)s_xfer_log.chunks, (unsigned)s_xfer.written_bytes,
                 (unsigned)s_xfer.expected_bytes);
#endif
}

void od_xfer_log_finish(void)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    char hex[64];

    log_hex(hex, sizeof hex, s_xfer_log.last_head, s_xfer_log.last_head_len);
    od_log_debug("DW final frame %u: %u bytes: %s", (unsigned)s_xfer_log.chunks,
                 (unsigned)s_xfer_log.last_len, hex);
#endif
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    /* Fixed-point formatting preserves the authority's two-decimal ratio/time and one-decimal
     * rate without relying on newlib-nano's optional long-long or floating-point printf support. */
    const uint32_t elapsed_ms = od_xfer_app_now_ms() - s_xfer.started_ms;
    const uint32_t elapsed_cs = (uint32_t)(((uint64_t)elapsed_ms + 5u) / 10u);
    char mode[64];

    if (s_xfer.compressed && s_xfer.received_bytes != 0u && s_xfer.written_bytes != 0u) {
        const uint32_t ratio = (uint32_t)(((uint64_t)s_xfer.written_bytes * 100u
                                          + s_xfer.received_bytes / 2u)
                                         / s_xfer.received_bytes);
        (void)snprintf(mode, sizeof mode, " zlib %u B on wire (%u.%02ux)",
                       (unsigned)s_xfer.received_bytes,
                       (unsigned)(ratio / 100u), (unsigned)(ratio % 100u));
    } else if (s_xfer.compressed) {
        (void)snprintf(mode, sizeof mode, " zlib %u B on wire",
                       (unsigned)s_xfer.received_bytes);
    } else {
        (void)snprintf(mode, sizeof mode, " raw");
    }
    if (elapsed_ms != 0u) {
        const uint32_t rate_tenths = (uint32_t)(((uint64_t)s_xfer.written_bytes * 10000u
                                                + (uint64_t)512u * elapsed_ms)
                                               / ((uint64_t)1024u * elapsed_ms));
        od_log_info("DW complete: %u chunks, %u/%u bytes,%s, %u.%02u s, "
                    "%u.%01u KB/s", (unsigned)s_xfer_log.chunks,
                    (unsigned)s_xfer.written_bytes, (unsigned)s_xfer.expected_bytes, mode,
                    (unsigned)(elapsed_cs / 100u), (unsigned)(elapsed_cs % 100u),
                    (unsigned)(rate_tenths / 10u), (unsigned)(rate_tenths % 10u));
    } else {
        od_log_info("DW complete: %u chunks, %u/%u bytes,%s, 0.00 s, n/a KB/s",
                    (unsigned)s_xfer_log.chunks, (unsigned)s_xfer.written_bytes,
                    (unsigned)s_xfer.expected_bytes, mode);
    }
#endif
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

    od_xfer_log_start();
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
    od_xfer_log_chunk(payload);
    s_xfer.received_bytes += (uint32_t)payload.n;
    if (s_xfer.compressed) {
        if (!od_xfer_stream_push(payload, false)) {
            return false;
        }
        od_xfer_log_progress();
        return true;
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
    od_xfer_log_progress();
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
    od_xfer_log_finish();
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
