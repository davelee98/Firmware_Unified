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
static reply_t g_replies[16];
static unsigned g_reply_n;
static int g_fail_reply;
static unsigned g_begin_full;
static unsigned g_begin_partial;
static unsigned g_abort;
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
static uint32_t g_etag;
static uint8_t g_scratch[64];
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

void od_xfer_app_prepare_start(void) { }
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
    if (offset != g_written) return 0u;
    ++g_write;
    g_written += (uint32_t)data.n;
    return (uint32_t)data.n;
}
od_mut_span_t od_xfer_app_inflate_scratch(void)
{ return od_mut_span_make(g_scratch, sizeof g_scratch); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; ++g_abort; }
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
    g_abort = 0u;
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
    g_etag = 0x11223344u;
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
    CHECK(g_reply_event < g_begin_event && g_begin_full == 1u);
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
    CHECK(od_pipe_data(&owner_data, od_span_make(seq1, sizeof seq1)) == OD_CMD_OK);
    CHECK(g_reply_n == 2u && g_replies[1].bytes[1] == 0x81u);
    CHECK(g_written == 0u);
    CHECK(od_pipe_data(&other_data, od_span_make(seq0, sizeof seq0)) == OD_CMD_NACK);
    CHECK(g_reply_n == 2u && g_written == 0u);
    CHECK(od_pipe_data(&owner_data, od_span_make(seq0, sizeof seq0)) == OD_CMD_OK);
    CHECK(g_written == 16u);

    CASE("non-BLE refusal is inert even while BLE owns a transfer");
    CHECK(od_pipe_data(&lan_data, od_span_make(seq0, sizeof seq0)) == OD_CMD_NACK);
    CHECK(g_reply_n == 3u && !g_replies[2].plain && g_replies[2].len == 4u
          && g_replies[2].bytes[0] == 0xFFu && g_replies[2].bytes[1] == 0x81u);
    CHECK(od_xfer_owns_hardware() && g_abort == 0u);
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
    test_bounds_and_fatal_reset();
    test_silent_refusals();
    test_reorder_owner_and_origin();
    test_start_failures_and_partial();
    test_completion_failures();
    printf("pipe: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
