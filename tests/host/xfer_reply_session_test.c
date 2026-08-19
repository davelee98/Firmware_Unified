#include "od_color.h"
#include "od_inflate_app.h"
#include "od_session_app.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "session_fake.h"

#include <stdio.h>
#include <string.h>

static struct od_session g_session;
static bool g_panel_ok;
static uint8_t g_scratch[32];
static uint8_t g_sent[OD_TX_FRAME_MAX];
static uint16_t g_sent_len;

struct od_session *od_session_app_state(void) { return &g_session; }
const struct SecurityConfig *od_session_app_security(void) { return &g_sec; }
uint32_t od_session_app_now_ms(void) { return 1000u; }
void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{ memcpy(out, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN); }
void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report)
{ (void)op; (void)result; (void)cmd; (void)report; }

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    g_sent_len = len;
    memcpy(g_sent, frame, len);
    return OD_RADIO_SENT;
}
bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{ (void)origin; (void)tag; return true; }
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{ (void)rp; (void)len; (void)why; }

void od_inflate_app_reset(uint32_t expected) { od_zlib_stream_reset(expected); }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ return od_zlib_stream_push(input.p, input.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ return od_zlib_stream_poll(out, cap, produced); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (!g_panel_ok || out == NULL) return false;
    out->width = 16u;
    out->height = 2u;
    out->partial_enabled = true;
    return od_color_direct_geometry(OD_COLOR_SCHEME_MONO, out->width, out->height,
                                    &out->geometry) == OD_COLOR_OK;
}
bool od_xfer_app_begin_full(const od_color_geometry_t *g) { return g != NULL; }
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; return false; }
uint32_t od_xfer_app_write(uint32_t offset, od_span_t data)
{ (void)offset; return (uint32_t)data.n; }
od_mut_span_t od_xfer_app_inflate_scratch(void)
{ return od_mut_span_make(g_scratch, sizeof g_scratch); }
void od_xfer_app_abort(void) { }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{ (void)owner; return OD_XFER_BARRIER_PROCEED; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; }
bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{ (void)mode; *completed = true; return true; }
uint32_t od_xfer_app_displayed_etag(void) { return 0u; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { (void)etag; }
uint32_t od_xfer_app_now_ms(void) { return 1000u; }

static int run_start(bool valid_panel)
{
    od_tx_reservation_t reservation;
    od_cmd_ctx_t ctx;

    memset(g_sent, 0, sizeof g_sent);
    g_sent_len = 0u;
    g_panel_ok = valid_panel;
    if (od_txq_reserve(1u, &reservation) != OD_TXQ_OK) return 0;
    ctx.rp.origin = OD_ORIGIN_BLE;
    ctx.rp.tag = 7u;
    ctx.r = &reservation;
    if (od_xfer_direct_start(&ctx, od_span_none())
        != (valid_panel ? OD_CMD_OK : OD_CMD_NACK)) return 0;
    od_txq_release(&reservation);
    (void)od_txq_process();
    return 1;
}

int main(void)
{
    uint8_t server_nonce[16];

    fake_reset();
    sec_init(0u);
    memset(&g_session, 0, sizeof g_session);
    od_session_init(&g_session, 0u);
    if (handshake(&g_session, 1000u, server_nonce, false) != OD_SESSION_AUTH_ESTABLISHED) {
        return 1;
    }
    od_txq_reset();
    od_xfer_reset();

    if (!run_start(true) || g_sent_len <= 2u || g_sent[0] != RESP_ACK
        || g_sent[1] != RESP_DIRECT_WRITE_START_ACK) {
        return 1;
    }
    od_xfer_reset();
    if (!run_start(false) || g_sent_len != 2u || g_sent[0] != RESP_NACK
        || g_sent[1] != RESP_DIRECT_WRITE_START_ACK) {
        return 1;
    }
    printf("xfer_reply_session: application ACK sealed, hard NACK plain\n");
    return 0;
}
