#include "od_cmd_test_ctx.h"
#include "od_inflate_app.h"
#include "od_pipe.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    bool plain;
    uint16_t len;
    uint8_t bytes[16];
} reply_t;

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case;
static reply_t g_replies[64];
static unsigned g_reply_n;
static int g_fail_reply;
static unsigned g_begin_full;
static unsigned g_begin_partial;
static unsigned g_prepare;
static unsigned g_abort;
static od_xfer_abort_reason_t g_abort_reason;
static unsigned g_write;
static unsigned g_refresh;
static unsigned g_barrier;
static unsigned g_event;
static unsigned g_reply_event;
static unsigned g_begin_event;
static bool g_begin_ok;
static bool g_refresh_ok;
static bool g_refresh_completed;
static od_xfer_barrier_t g_barrier_result;
static uint32_t g_written;
static uint32_t g_write_limit;
static uint32_t g_etag;
static uint8_t g_scratch[64];
static uint8_t g_written_data[512];
static od_tx_reservation_t g_reservation;

#define CASE(name) (g_case = (name))
#define CHECK(cond) do {                                                        \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
        ++g_failures;                                                          \
        printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);    \
    }                                                                          \
} while (0)

static od_txq_status_t record(bool plain, const uint8_t *frame, uint16_t len)
{
    const unsigned call = g_reply_n;

    if (call < sizeof g_replies / sizeof g_replies[0]) {
        g_replies[call].plain = plain;
        g_replies[call].len = len;
        if (len <= sizeof g_replies[call].bytes) {
            memcpy(g_replies[call].bytes, frame, len);
        }
    }
    ++g_reply_n;
    g_reply_event = ++g_event;
    return g_fail_reply == (int)call ? OD_TXQ_SEAL_FAILED : OD_TXQ_OK;
}

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    (void)r; (void)rp;
    return record(false, frame, len);
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    (void)r; (void)rp;
    return record(true, frame, len);
}

void od_inflate_app_reset(uint32_t expected) { od_zlib_stream_reset(expected); }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ return od_zlib_stream_push(input.p, input.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ return od_zlib_stream_poll(out, cap, produced); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

void od_xfer_app_prepare_start(void) { ++g_prepare; }
bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof *out);
    out->width = 32u;
    out->height = 8u;
    out->partial_enabled = true;
    return od_color_direct_geometry(OD_COLOR_SCHEME_MONO, out->width, out->height,
                                    &out->geometry) == OD_COLOR_OK;
}
bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
    (void)geometry;
    ++g_begin_full;
    g_begin_event = ++g_event;
    return g_begin_ok;
}
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                               uint32_t plane_bytes)
{
    (void)x; (void)y; (void)width; (void)height; (void)plane_bytes;
    ++g_begin_partial;
    g_begin_event = ++g_event;
    return g_begin_ok;
}
uint32_t od_xfer_app_write(uint32_t offset, od_span_t data)
{
    if (offset != g_written || data.n > g_write_limit
        || offset + data.n > sizeof g_written_data) return 0u;
    ++g_write;
    memcpy(g_written_data + offset, data.p, data.n);
    g_written += (uint32_t)data.n;
    return (uint32_t)data.n;
}
od_mut_span_t od_xfer_app_inflate_scratch(void)
{ return od_mut_span_make(g_scratch, sizeof g_scratch); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{ g_abort_reason = reason; ++g_abort; }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{ (void)owner; ++g_barrier; return g_barrier_result; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; ++g_abort; }
bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    (void)mode;
    ++g_refresh;
    if (!g_refresh_ok || completed == NULL) return false;
    *completed = g_refresh_completed;
    return true;
}
uint32_t od_xfer_app_displayed_etag(void) { return g_etag; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { g_etag = etag; }
uint32_t od_xfer_app_now_ms(void) { return 1234u; }

static void setup(void)
{
    od_xfer_reset();
    memset(g_replies, 0, sizeof g_replies);
    g_reply_n = 0u;
    g_fail_reply = -1;
    g_begin_full = 0u;
    g_begin_partial = 0u;
    g_prepare = 0u;
    g_abort = 0u;
    g_abort_reason = OD_XFER_ABORT_REPLACED;
    g_write = 0u;
    g_refresh = 0u;
    g_barrier = 0u;
    g_event = 0u;
    g_reply_event = 0u;
    g_begin_event = 0u;
    g_begin_ok = true;
    g_refresh_ok = true;
    g_refresh_completed = true;
    g_barrier_result = OD_XFER_BARRIER_PROCEED;
    g_written = 0u;
    g_write_limit = UINT32_MAX;
    g_etag = 0x11223344u;
    memset(g_written_data, 0, sizeof g_written_data);
    memset(&g_reservation, 0, sizeof g_reservation);
}

static od_span_t start_body(uint8_t out[22], uint8_t flags, uint8_t w, uint8_t n,
                            uint16_t frame, uint32_t total)
{
    memset(out, 0, 22u);
    out[0] = PIPE_VERSION;
    out[1] = flags;
    out[2] = w;
    out[3] = n;
    out[4] = (uint8_t)frame;
    out[5] = (uint8_t)(frame >> 8);
    out[6] = (uint8_t)total;
    out[7] = (uint8_t)(total >> 8);
    out[8] = (uint8_t)(total >> 16);
    out[9] = (uint8_t)(total >> 24);
    return od_span_make(out, 10u);
}

static uint32_t adler32(od_span_t bytes)
{
    uint32_t a = 1u;
    uint32_t b = 0u;
    size_t i;

    for (i = 0u; i < bytes.n; ++i) {
        a = (a + bytes.p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static size_t make_stored(od_span_t plain, uint8_t *compressed, size_t capacity)
{
    uint16_t len;
    uint16_t inverse;
    const uint32_t checksum = adler32(plain);

    if (plain.n > UINT16_MAX || capacity < plain.n + 11u) {
        return 0u;
    }
    len = (uint16_t)plain.n;
    inverse = (uint16_t)~len;
    compressed[0] = 0x18u;
    compressed[1] = 0x19u;
    compressed[2] = 0x01u;
    compressed[3] = (uint8_t)len;
    compressed[4] = (uint8_t)(len >> 8);
    compressed[5] = (uint8_t)inverse;
    compressed[6] = (uint8_t)(inverse >> 8);
    memcpy(compressed + 7u, plain.p, plain.n);
    compressed[7u + plain.n] = (uint8_t)(checksum >> 24);
    compressed[8u + plain.n] = (uint8_t)(checksum >> 16);
    compressed[9u + plain.n] = (uint8_t)(checksum >> 8);
    compressed[10u + plain.n] = (uint8_t)checksum;
    return plain.n + 11u;
}

static od_cmd_result_t send_data(const od_reply_t *owner, uint8_t seq,
                                 const uint8_t *payload, size_t payload_n)
{
    uint8_t frame[1u + OD_PIPE_REORDER_PAYLOAD];

    if (payload_n > OD_PIPE_REORDER_PAYLOAD) {
        return OD_CMD_NACK;
    }
    frame[0] = seq;
    if (payload_n > 0u) {
        memcpy(frame + 1u, payload, payload_n);
    }
    od_cmd_ctx_t ctx = od_test_cmd_ctx(*owner, &g_reservation,
                                       (uint16_t)(3u + payload_n), false);
    return od_pipe_data(&ctx, od_span_make(frame, payload_n + 1u));
}

static void test_start_and_auto_end(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 12u, false);
    uint8_t sb[22];
    uint8_t data[33] = { 0u };
    od_cmd_ctx_t data_ctx = od_test_cmd_ctx(owner, &g_reservation, 35u, false);

    CASE("START ACK precedes activation; full raw DATA auto-completes");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 32u, 32u, 244u, 32u)) == OD_CMD_OK);
    CHECK(g_reply_n == 1u && g_replies[0].len == 8u);
    CHECK(g_replies[0].bytes[0] == 0u && g_replies[0].bytes[1] == 0x80u);
    CHECK(g_reply_event < g_begin_event && g_begin_full == 1u && g_prepare == 1u);
    CHECK(od_xfer_owns_hardware() && od_xfer_frames_may_arrive());

    data[0] = 0u;
    CHECK(od_pipe_data(&data_ctx, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(g_written == 32u && g_write == 1u);
    CHECK(g_reply_n == 4u);
    CHECK(g_replies[1].bytes[1] == 0x81u);
    CHECK(g_replies[2].bytes[1] == 0x82u);
    CHECK(g_replies[3].bytes[1] == RESP_DIRECT_WRITE_REFRESH_SUCCESS);
    CHECK(g_barrier == 1u && g_refresh == 1u && !od_xfer_frames_may_arrive());
}

static void test_start_validation(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 24u, false);
    uint8_t sb[23];

    CASE("START rejects version, unknown flags and mismatched total with exact errors");
    setup();
    (void)start_body(sb, 0u, 4u, 2u, 244u, 32u);
    sb[0] = 0x7fu;
    CHECK(od_pipe_start(&start, od_span_make(sb, 10u)) == OD_CMD_NACK);
    CHECK(g_replies[0].plain && g_replies[0].bytes[2] == OD_ERR_PIPE_START_BAD_HEADER);

    setup();
    (void)start_body(sb, 0x40u, 4u, 2u, 244u, 32u);
    CHECK(od_pipe_start(&start, od_span_make(sb, 10u)) == OD_CMD_NACK);
    CHECK(g_replies[0].bytes[2] == OD_ERR_PIPE_START_UNKNOWN_FLAG);

    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 2u, 244u, 31u)) == OD_CMD_NACK);
    CHECK(g_replies[0].bytes[2] == OD_ERR_PIPE_START_SIZE_MISMATCH);

    CASE("partial START requires its extension and admits compression without a config gate");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, PIPE_FLAG_PARTIAL, 4u, 2u, 244u, 16u))
          == OD_CMD_NACK);
    CHECK(g_replies[0].bytes[2] == OD_ERR_PIPE_START_BAD_HEADER && g_prepare == 0u);

    setup();
    CHECK(od_pipe_start(&start,
                        start_body(sb, PIPE_FLAG_PARTIAL | 0x40u,
                                   4u, 2u, 244u, 16u)) == OD_CMD_NACK);
    CHECK(g_replies[0].bytes[2] == OD_ERR_PIPE_START_UNKNOWN_FLAG
          && g_prepare == 0u && g_begin_partial == 0u);

    setup();
    (void)start_body(sb, PIPE_FLAG_PARTIAL | PIPE_FLAG_COMPRESSED,
                     4u, 2u, 244u, 16u);
    sb[10] = 0x44u; sb[11] = 0x33u; sb[12] = 0x22u; sb[13] = 0x11u;
    sb[14] = 0u; sb[15] = 0u;
    sb[16] = 0u; sb[17] = 0u;
    sb[18] = 8u; sb[19] = 0u;
    sb[20] = 8u; sb[21] = 0u;
    sb[22] = 0xA5u;
    CHECK(od_pipe_start(&start, od_span_make(sb, sizeof sb)) == OD_CMD_OK);
    CHECK(g_prepare == 1u && g_begin_partial == 1u
          && (g_replies[0].bytes[7] & PIPE_FLAG_PARTIAL) != 0u);
    {
        const uint8_t invalid_stream[] = { 0u, 0u, 0u };
        CHECK(send_data(&owner, 0u, invalid_stream, sizeof invalid_stream) == OD_CMD_NACK);
        CHECK(g_replies[1].plain && g_replies[1].bytes[2] == 0x02u);
    }
}

static void test_bounds_and_fatal_reset(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 12u, false);
    od_cmd_ctx_t bad_data = od_test_cmd_ctx(owner, &g_reservation, 2u, false);
    uint8_t sb[22];
    uint8_t data[] = { 0u, 0xAAu };

    CASE("negotiated bounds fail fatal and fatal reset does not abort twice");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 4u, 4u, 32u)) == OD_CMD_OK);
    CHECK(od_pipe_data(&bad_data, od_span_make(data, sizeof data)) == OD_CMD_NACK);
    CHECK(g_reply_n == 2u && g_replies[1].plain && g_replies[1].bytes[0] == 0xFFu
          && g_replies[1].bytes[1] == 0x81u && g_replies[1].bytes[2] == 0x03u);
    CHECK(!od_xfer_owns_hardware() && od_xfer_frames_may_arrive() && g_abort == 1u);
    od_xfer_reset();
    CHECK(!od_xfer_frames_may_arrive() && g_abort == 1u);

    CASE("START minimum is protection-aware");
    setup();
    start = od_test_cmd_ctx(owner, &g_reservation, 12u, true);
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 1u, 1u, 32u, 32u)) == OD_CMD_NACK);
    CHECK(g_replies[0].plain && g_replies[0].bytes[2] == OD_ERR_PIPE_START_BAD_HEADER);
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 1u, 1u, 33u, 32u)) == OD_CMD_OK);

    CASE("protected DATA is bounded by its original sealed wire length");
    setup();
    start = od_test_cmd_ctx(owner, &g_reservation, 41u, true);
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 1u, 1u, 33u, 32u)) == OD_CMD_OK);
    {
        uint8_t protected_data[] = { 0u, 0xAAu };
        od_cmd_ctx_t over = od_test_cmd_ctx(owner, &g_reservation, 34u, true);
        CHECK(od_pipe_data(&over, od_span_make(protected_data, sizeof protected_data))
              == OD_CMD_NACK);
        CHECK(g_replies[1].plain && g_replies[1].bytes[2] == 0x03u && g_abort == 1u);
    }
}

static void test_silent_refusals(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    const od_reply_t other = { OD_ORIGIN_BLE, 8u };
    od_cmd_ctx_t owner_ctx = od_test_cmd_ctx(owner, &g_reservation, 3u, false);
    od_cmd_ctx_t other_ctx = od_test_cmd_ctx(other, &g_reservation, 3u, false);
    uint8_t sb[22];
    uint8_t data[] = { 0u };

    CASE("inactive and empty DATA are silent; inactive END answers plainly");
    setup();
    CHECK(od_pipe_data(&owner_ctx, od_span_make(data, sizeof data)) == OD_CMD_NACK);
    CHECK(od_pipe_data(&owner_ctx, od_span_none()) == OD_CMD_NACK);
    CHECK(g_reply_n == 0u);
    CHECK(od_pipe_end(&other_ctx, od_span_none()) == OD_CMD_NACK);
    CHECK(g_reply_n == 1u && g_replies[0].plain && g_replies[0].len == 2u
          && g_replies[0].bytes[0] == RESP_NACK && g_replies[0].bytes[1] == 0x82u);

    CASE("wrong-owner DATA and END are inert against an open transfer");
    setup();
    CHECK(od_pipe_start(&owner_ctx, start_body(sb, 0u, 2u, 2u, 244u, 32u)) == OD_CMD_OK);
    CHECK(od_pipe_data(&other_ctx, od_span_make(data, sizeof data)) == OD_CMD_NACK);
    CHECK(od_pipe_end(&other_ctx, od_span_none()) == OD_CMD_NACK);
    CHECK(g_reply_n == 1u && od_xfer_owns_hardware() && g_abort == 0u);

    CASE("fatal DATA is silent and owner END retires the latch without a second abort");
    {
        uint8_t outside[] = { 100u, 0xAAu };
        CHECK(od_pipe_data(&owner_ctx, od_span_make(outside, sizeof outside)) == OD_CMD_NACK);
        CHECK(g_abort == 1u && od_xfer_frames_may_arrive());
        {
            const unsigned replies = g_reply_n;
            CHECK(od_pipe_data(&owner_ctx, od_span_make(data, sizeof data)) == OD_CMD_NACK);
            CHECK(g_reply_n == replies);
            CHECK(od_pipe_end(&owner_ctx, od_span_none()) == OD_CMD_NACK);
            CHECK(g_reply_n == replies + 1u && g_replies[replies].plain
                  && g_replies[replies].len == 2u && g_abort == 1u
                  && !od_xfer_frames_may_arrive());
        }
    }
}

static void test_reorder_owner_and_origin(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    const od_reply_t other = { OD_ORIGIN_BLE, 8u };
    const od_reply_t lan = { OD_ORIGIN_LAN_TLS, 1u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 12u, false);
    od_cmd_ctx_t owner_data = od_test_cmd_ctx(owner, &g_reservation, 11u, false);
    od_cmd_ctx_t other_data = od_test_cmd_ctx(other, &g_reservation, 11u, false);
    od_cmd_ctx_t lan_data = od_test_cmd_ctx(lan, &g_reservation, 11u, false);
    uint8_t sb[22];
    uint8_t seq1[9] = { 1u };
    uint8_t seq0[9] = { 0u };

    CASE("ahead frame SACKs, owner closes the gap, other owner is inert");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 4u, 244u, 32u)) == OD_CMD_OK);
    CHECK(!od_xfer_log_quiet(CMD_PIPE_WRITE_DATA));
    CHECK(od_pipe_data(&owner_data, od_span_make(seq1, sizeof seq1)) == OD_CMD_OK);
    CHECK(g_reply_n == 2u && g_replies[1].bytes[1] == 0x81u);
    CHECK(g_written == 0u);
    CHECK(!od_xfer_log_quiet(CMD_PIPE_WRITE_DATA));
    CHECK(od_pipe_data(&other_data, od_span_make(seq0, sizeof seq0)) == OD_CMD_NACK);
    CHECK(g_reply_n == 2u && g_written == 0u);
    CHECK(od_pipe_data(&owner_data, od_span_make(seq0, sizeof seq0)) == OD_CMD_OK);
    CHECK(g_written == 16u);
    CHECK(od_xfer_log_quiet(CMD_PIPE_WRITE_DATA));

    CASE("non-BLE refusal is inert even while BLE owns a transfer");
    CHECK(od_pipe_data(&lan_data, od_span_make(seq0, sizeof seq0)) == OD_CMD_NACK);
    CHECK(g_reply_n == 3u && !g_replies[2].plain && g_replies[2].len == 4u
          && g_replies[2].bytes[0] == 0xFFu && g_replies[2].bytes[1] == 0x81u);
    CHECK(od_xfer_owns_hardware() && g_abort == 0u);
}

static void test_cadence_sack_reorder_and_wrap(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 12u, false);
    uint8_t sb[22];
    uint8_t payload[8];

    CASE("in-order SACK cadence pins highest sequence and mask bytes");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 2u, 244u, 32u)) == OD_CMD_OK);
    memset(payload, 0u, sizeof payload);
    CHECK(send_data(&owner, 0u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_reply_n == 1u);
    memset(payload, 1u, sizeof payload);
    CHECK(send_data(&owner, 1u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_reply_n == 2u && g_replies[1].len == 7u);
    CHECK(g_replies[1].bytes[2] == 1u && g_replies[1].bytes[3] == 1u
          && g_replies[1].bytes[4] == 0u && g_replies[1].bytes[5] == 0u
          && g_replies[1].bytes[6] == 0u);

    CASE("gap cadence, exact SACK mask and multi-slot drain preserve stream order");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 2u, 244u, 32u)) == OD_CMD_OK);
    memset(payload, 2u, sizeof payload);
    CHECK(send_data(&owner, 2u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_reply_n == 2u && g_replies[1].bytes[2] == 2u
          && g_replies[1].bytes[3] == 0u);
    memset(payload, 1u, sizeof payload);
    CHECK(send_data(&owner, 1u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_reply_n == 2u);
    CHECK(send_data(&owner, 1u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_reply_n == 3u && g_replies[2].bytes[2] == 2u
          && g_replies[2].bytes[3] == 1u);
    memset(payload, 0u, sizeof payload);
    CHECK(send_data(&owner, 0u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_written == 24u && g_write == 3u);
    CHECK(g_replies[3].bytes[2] == 2u && g_replies[3].bytes[3] == 3u);
    CHECK(g_written_data[0] == 0u && g_written_data[8] == 1u
          && g_written_data[16] == 2u);
    memset(payload, 3u, sizeof payload);
    CHECK(send_data(&owner, 3u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_written == 32u && g_refresh == 1u);

    CASE("sequence arithmetic crosses 255 to 0 without losing the transfer");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 32u, 32u, 244u, 32u)) == OD_CMD_OK);
    {
        unsigned i;
        for (i = 0u; i <= 256u; ++i) {
            CHECK(send_data(&owner, (uint8_t)i, NULL, 0u) == OD_CMD_OK);
        }
    }
    CHECK(g_abort == 0u && od_xfer_owns_hardware());
    CHECK(send_data(&owner, 64u, payload, 1u) == OD_CMD_NACK);
    CHECK(g_replies[g_reply_n - 1u].plain
          && g_replies[g_reply_n - 1u].bytes[2] == 0x04u && g_abort == 1u);
}

static void test_consume_and_reply_failures(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 12u, false);
    uint8_t sb[22];
    uint8_t payload[40];

    CASE("raw trailing bytes are ignored after the declared image is complete");
    setup();
    memset(payload, 0xA5u, sizeof payload);
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 4u, 244u, 32u)) == OD_CMD_OK);
    CHECK(send_data(&owner, 0u, payload, sizeof payload) == OD_CMD_OK);
    CHECK(g_written == 32u && g_write == 1u && g_abort == 0u && g_refresh == 1u);

    CASE("raw partial trailing bytes are fatal instead of truncated");
    setup();
    (void)start_body(sb, PIPE_FLAG_PARTIAL, 4u, 4u, 244u, 16u);
    sb[10] = 0x44u; sb[11] = 0x33u; sb[12] = 0x22u; sb[13] = 0x11u;
    sb[18] = 8u;
    sb[20] = 8u;
    CHECK(od_pipe_start(&start, od_span_make(sb, sizeof sb)) == OD_CMD_OK);
    CHECK(send_data(&owner, 0u, payload, 20u) == OD_CMD_NACK);
    CHECK(g_written == 0u && g_abort == 1u
          && g_abort_reason == OD_XFER_ABORT_STREAM_FAILED);
    CHECK(g_replies[1].plain && g_replies[1].bytes[2] == 0x03u && g_etag == 0u);

    CASE("raw sink refusal selects fatal 0x03");
    setup();
    g_write_limit = 7u;
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 4u, 244u, 32u)) == OD_CMD_OK);
    CHECK(send_data(&owner, 0u, payload, 8u) == OD_CMD_NACK);
    CHECK(g_replies[1].plain && g_replies[1].bytes[2] == 0x03u && g_abort == 1u);

    CASE("compressed stream failure selects fatal 0x02");
    setup();
    CHECK(od_pipe_start(&start,
                        start_body(sb, PIPE_FLAG_COMPRESSED, 4u, 4u, 244u, 32u))
          == OD_CMD_OK);
    CHECK(send_data(&owner, 0u, payload, 3u) == OD_CMD_NACK);
    CHECK(g_replies[1].plain && g_replies[1].bytes[2] == 0x02u && g_abort == 1u);

    CASE("cadence, gap and duplicate SACK substitution each release hardware once");
    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 2u, 244u, 32u)) == OD_CMD_OK);
    g_fail_reply = 1;
    CHECK(send_data(&owner, 0u, payload, 8u) == OD_CMD_OK);
    CHECK(send_data(&owner, 1u, payload, 8u) == OD_CMD_NACK);
    CHECK(g_reply_n == 2u && g_abort == 1u && od_xfer_frames_may_arrive());

    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 2u, 244u, 32u)) == OD_CMD_OK);
    g_fail_reply = 1;
    CHECK(send_data(&owner, 1u, payload, 8u) == OD_CMD_NACK);
    CHECK(g_reply_n == 2u && g_abort == 1u && od_xfer_frames_may_arrive());

    setup();
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 4u, 4u, 244u, 32u)) == OD_CMD_OK);
    CHECK(send_data(&owner, 0u, payload, 8u) == OD_CMD_OK);
    g_fail_reply = 1;
    CHECK(send_data(&owner, 0u, payload, 8u) == OD_CMD_NACK);
    CHECK(g_reply_n == 2u && g_abort == 1u && od_xfer_frames_may_arrive());
}

static void test_compressed_full(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 12u, false);
    od_cmd_ctx_t end = od_test_cmd_ctx(owner, &g_reservation, 2u, false);
    uint8_t sb[22];
    uint8_t plain[32];
    uint8_t compressed[64];
    size_t compressed_n;

    CASE("compressed full PIPE is admitted, finalized explicitly and refreshed");
    setup();
    memset(plain, 0x5Au, sizeof plain);
    compressed_n = make_stored(od_span_make(plain, sizeof plain), compressed,
                               sizeof compressed);
    CHECK(compressed_n > 0u);
    CHECK(od_pipe_start(&start,
                        start_body(sb, PIPE_FLAG_COMPRESSED, 4u, 4u, 244u, 32u))
          == OD_CMD_OK);
    CHECK(g_prepare == 1u);
    CHECK(send_data(&owner, 0u, compressed, compressed_n) == OD_CMD_OK);
    CHECK(g_written == sizeof plain && g_refresh == 0u && od_xfer_owns_hardware());
    CHECK(od_pipe_end(&end, od_span_none()) == OD_CMD_OK);
    CHECK(g_refresh == 1u && !od_xfer_frames_may_arrive());
    CHECK(memcmp(g_written_data, plain, sizeof plain) == 0);
}

static void test_start_failures_and_partial(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t start = od_test_cmd_ctx(owner, &g_reservation, 24u, false);
    uint8_t sb[22];

    CASE("substituted START ACK unwinds without activation");
    setup();
    g_fail_reply = 0;
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 1u, 1u, 244u, 32u)) == OD_CMD_NACK);
    CHECK(g_begin_full == 0u && g_abort == 1u && !od_xfer_frames_may_arrive());

    CASE("post-ACK activation failure enters fatal without contradictory NACK");
    setup();
    g_begin_ok = false;
    CHECK(od_pipe_start(&start, start_body(sb, 0u, 1u, 1u, 244u, 32u)) == OD_CMD_OK);
    CHECK(g_reply_n == 1u && g_abort == 1u && !od_xfer_owns_hardware()
          && od_xfer_frames_may_arrive());

    CASE("partial etag is little-endian on START and big-endian on END");
    setup();
    (void)start_body(sb, PIPE_FLAG_PARTIAL, 1u, 1u, 244u, 16u);
    sb[10] = 0x44u; sb[11] = 0x33u; sb[12] = 0x22u; sb[13] = 0x11u;
    sb[14] = 0u; sb[15] = 0u;
    sb[16] = 0u; sb[17] = 0u;
    sb[18] = 8u; sb[19] = 0u;
    sb[20] = 8u; sb[21] = 0u;
    CHECK(od_pipe_start(&start, od_span_make(sb, sizeof sb)) == OD_CMD_OK);
    CHECK(g_begin_partial == 1u);
    {
        uint8_t data[17] = { 0u };
        uint8_t end[] = { 2u, 0xA1u, 0xB2u, 0xC3u, 0xD4u };
        od_cmd_ctx_t data_ctx = od_test_cmd_ctx(owner, &g_reservation, 19u, false);
        od_cmd_ctx_t end_ctx = od_test_cmd_ctx(owner, &g_reservation, 7u, false);
        CHECK(od_pipe_data(&data_ctx, od_span_make(data, sizeof data)) == OD_CMD_OK);
        CHECK(od_pipe_end(&end_ctx, od_span_make(end, sizeof end)) == OD_CMD_OK);
        CHECK(g_etag == 0xA1B2C3D4u && g_refresh == 1u);
    }
}

static void start_partial_and_data(const od_reply_t *owner, uint8_t sb[22])
{
    uint8_t data[17] = { 0u };
    od_cmd_ctx_t start = od_test_cmd_ctx(*owner, &g_reservation, 24u, false);
    od_cmd_ctx_t data_ctx = od_test_cmd_ctx(*owner, &g_reservation, 19u, false);

    (void)start_body(sb, PIPE_FLAG_PARTIAL, 1u, 1u, 244u, 16u);
    sb[10] = 0x44u; sb[11] = 0x33u; sb[12] = 0x22u; sb[13] = 0x11u;
    sb[14] = 0u; sb[15] = 0u;
    sb[16] = 0u; sb[17] = 0u;
    sb[18] = 8u; sb[19] = 0u;
    sb[20] = 8u; sb[21] = 0u;
    CHECK(od_pipe_start(&start, od_span_make(sb, 22u)) == OD_CMD_OK);
    CHECK(od_pipe_data(&data_ctx, od_span_make(data, sizeof data)) == OD_CMD_OK);
}

static void test_completion_failures(void)
{
    const od_reply_t owner = { OD_ORIGIN_BLE, 7u };
    od_cmd_ctx_t end = od_test_cmd_ctx(owner, &g_reservation, 7u, false);
    const uint8_t end_body[] = { 2u, 0xA1u, 0xB2u, 0xC3u, 0xD4u };
    uint8_t sb[22];

    CASE("incomplete PIPE END uses the force-off-specific abort reason");
    setup();
    {
        od_cmd_ctx_t full_start = od_test_cmd_ctx(owner, &g_reservation, 12u, false);
        uint8_t short_data[8] = { 0u };
        CHECK(od_pipe_start(&full_start,
                            start_body(sb, 0u, 4u, 4u, 244u, 32u)) == OD_CMD_OK);
        CHECK(send_data(&owner, 0u, short_data, sizeof short_data) == OD_CMD_OK);
        CHECK(od_pipe_end(&end, od_span_make(end_body, sizeof end_body)) == OD_CMD_NACK);
        CHECK(g_abort == 1u && g_abort_reason == OD_XFER_ABORT_PIPE_INCOMPLETE
              && !od_xfer_frames_may_arrive());
    }

    CASE("END ACK substitution aborts before barrier and refresh");
    setup();
    start_partial_and_data(&owner, sb);
    g_fail_reply = 3;
    CHECK(od_pipe_end(&end, od_span_make(end_body, sizeof end_body)) == OD_CMD_NACK);
    CHECK(g_reply_n == 4u && g_barrier == 0u && g_refresh == 0u && g_abort == 1u
          && g_etag == 0u && !od_xfer_frames_may_arrive());

    CASE("barrier abort runs recovery once and emits no completion status");
    setup();
    start_partial_and_data(&owner, sb);
    g_barrier_result = OD_XFER_BARRIER_ABORT;
    CHECK(od_pipe_end(&end, od_span_make(end_body, sizeof end_body)) == OD_CMD_NACK);
    CHECK(g_reply_n == 4u && g_barrier == 1u && g_refresh == 0u && g_abort == 1u
          && g_etag == 0u && !od_xfer_frames_may_arrive());

    CASE("refresh failure follows END ACK with plaintext END NACK and clears etag");
    setup();
    start_partial_and_data(&owner, sb);
    g_refresh_ok = false;
    CHECK(od_pipe_end(&end, od_span_make(end_body, sizeof end_body)) == OD_CMD_NACK);
    CHECK(g_reply_n == 5u && g_replies[4].plain && g_replies[4].len == 2u
          && g_replies[4].bytes[0] == RESP_NACK && g_replies[4].bytes[1] == 0x82u
          && g_abort == 1u && g_etag == 0u && !od_xfer_frames_may_arrive());

    CASE("refresh timeout is accepted and reports 0x74");
    setup();
    start_partial_and_data(&owner, sb);
    g_refresh_completed = false;
    CHECK(od_pipe_end(&end, od_span_make(end_body, sizeof end_body)) == OD_CMD_OK);
    CHECK(g_reply_n == 5u && !g_replies[4].plain && g_replies[4].bytes[0] == RESP_ACK
          && g_replies[4].bytes[1] == RESP_DIRECT_WRITE_REFRESH_TIMEOUT
          && g_etag == 0u && !od_xfer_frames_may_arrive());

    CASE("substituted refresh status makes the END verdict a NACK");
    setup();
    start_partial_and_data(&owner, sb);
    g_fail_reply = 4;
    CHECK(od_pipe_end(&end, od_span_make(end_body, sizeof end_body)) == OD_CMD_NACK);
    CHECK(g_reply_n == 5u && g_refresh == 1u && !od_xfer_frames_may_arrive());
}

int main(void)
{
    test_start_and_auto_end();
    test_start_validation();
    test_bounds_and_fatal_reset();
    test_silent_refusals();
    test_reorder_owner_and_origin();
    test_cadence_sack_reorder_and_wrap();
    test_consume_and_reply_failures();
    test_compressed_full();
    test_start_failures_and_partial();
    test_completion_failures();
    printf("pipe: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
