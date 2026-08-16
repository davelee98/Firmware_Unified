/* pipe_write_test.c -- targets/nordic-zephyr/src/opendisplay_pipe_write.cpp, the PRODUCTION file,
 * against fake display, reply and Zephyr seams.
 *
 * VERDICTS ARE POLICY, WHICH IS WHY THIS SUITE EXISTS SEPARATELY FROM THE WIRE. The bytes a PIPE
 * command sends are not the whole of its effect: the od_cmd_result_t it reports decides whether
 * the frame stamps the session's activity clock. A wrong OK on a refused frame keeps a link alive
 * on traffic the device rejected, and it is invisible in a packet capture -- both sides see the
 * same NACK. So each case asserts the verdict AND the frames, and a wrong pairing fails.
 *
 * WHAT IT DOES NOT COVER. The reorder window, the SACK mask arithmetic and the compression path
 * are unchanged by this commit and are not re-derived here; the panel is a counter, so nothing
 * about real refresh timing is proven. Hardware still owns the encrypted PIPE replay case.
 */

#include "opendisplay_pipe_write.h"

#include "od_cmd.h"
#include "od_txq.h"
#include "opendisplay_display.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"
#include "zephyr/kernel.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)

#define CASE(name) (g_case = (name))

/* ------------------------------------------------------------------------------ fake reply --- */

#define SENT_MAX 32u
static struct { uint16_t len; bool plain; uint8_t data[64]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static unsigned g_flushes;

/* Make the Nth od_cmd_reply() (the SEALED path, counting from 1) report a substitution. That is
 * od_reply()'s "I could not seal this, so I sent a plaintext hard NACK instead" -- the caller must
 * then stop, because a success queued behind a refusal contradicts it on the wire. 0 = never. */
static unsigned g_reply_fail_on;
static unsigned g_reply_calls;

static void record(const uint8_t *frame, uint16_t len, bool plain)
{
    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].len = len;
        g_sent[g_sent_n].plain = plain;
        memcpy(g_sent[g_sent_n].data, frame, (len < sizeof g_sent[0].data) ? len
                                                                           : sizeof g_sent[0].data);
        ++g_sent_n;
    }
}

od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    (void)ctx;
    ++g_reply_calls;
    if (g_reply_fail_on == g_reply_calls) {
        /* od_reply substitutes the NACK itself, so the frame the caller asked for never goes out.
         * Recording nothing here is what makes "no success was queued behind the refusal"
         * checkable. */
        return OD_TXQ_SEAL_FAILED;
    }
    record(frame, len, false);
    return OD_TXQ_OK;
}

od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    (void)ctx;
    record(frame, len, true);
    return OD_TXQ_OK;
}

void od_cmd_flush_before_refresh(void) { ++g_flushes; }

/* ---------------------------------------------------------------------------- fake display --- */

static uint32_t g_dw_total;         /* bytes the transfer is expected to carry */
static uint32_t g_dw_written;
static bool     g_dw_active;
static bool     g_partial_active;
static unsigned g_aborts;
static unsigned g_etag_clears;
static unsigned g_prepares;
static unsigned g_refreshes;
static int      g_data_rc;          /* what direct_write_data returns */
static int      g_prepare_rc;
static int      g_refresh_rc;
static bool     g_refresh_ok = true;
static int      g_full_start_rc;
static int      g_partial_arm_rc;
static uint8_t  g_partial_arm_err = OD_ERR_PIPE_START_BAD_HEADER;
static int      g_partial_prepare_rc;

int opendisplay_display_direct_write_start(const uint8_t *p, uint16_t n) { (void)p; (void)n; return 0; }

int opendisplay_display_direct_write_data(const uint8_t *p, uint16_t n)
{
    (void)p;
    if (g_data_rc != 0) {
        return g_data_rc;
    }
    g_dw_written += n;
    return 0;
}

int opendisplay_display_direct_write_end_prepare(const uint8_t *p, uint16_t n)
{
    (void)p; (void)n;
    ++g_prepares;
    return g_prepare_rc;
}

int opendisplay_display_direct_write_end_refresh(const uint8_t *p, uint16_t n, bool *refresh_ok)
{
    (void)p; (void)n;
    ++g_refreshes;
    if (refresh_ok != NULL) {
        *refresh_ok = g_refresh_ok;
    }
    return g_refresh_rc;
}

int opendisplay_display_partial_write_start(const uint8_t *p, uint16_t n, uint8_t *err)
{
    (void)p; (void)n; (void)err;
    return 0;
}

bool     opendisplay_display_partial_active(void)         { return g_partial_active; }
bool     opendisplay_display_dw_active(void)              { return g_dw_active; }
uint32_t opendisplay_display_bytes_written(void)          { return g_dw_written; }
uint32_t opendisplay_display_total_bytes(void)            { return g_dw_total; }
uint32_t opendisplay_display_expected_dw_bytes(void)      { return g_dw_total; }
uint32_t opendisplay_display_displayed_etag(void)         { return 0u; }
void     opendisplay_display_clear_etag(void)             { ++g_etag_clears; }
void     opendisplay_display_set_partial_new_etag(uint32_t e) { (void)e; }
uint32_t opendisplay_display_partial_bytes_written(void)  { return g_dw_written; }
uint32_t opendisplay_display_partial_expected(void)       { return g_dw_total; }
bool     opendisplay_display_partial_compressed(void)     { return false; }
uint32_t opendisplay_display_calc_plane_bytes(uint16_t w, uint16_t h) { return (uint32_t)w * h; }
int      opendisplay_display_pipe_full_start(bool c, uint32_t n) { (void)c; (void)n; return g_full_start_rc; }

int opendisplay_display_pipe_partial_arm(uint8_t flags, uint32_t old_etag, uint16_t x, uint16_t y,
                                         uint16_t w, uint16_t h, uint32_t total, uint8_t *err)
{
    (void)flags; (void)old_etag; (void)x; (void)y; (void)w; (void)h; (void)total;
    if (g_partial_arm_rc != 0 && err != NULL) {
        *err = g_partial_arm_err;
    }
    return g_partial_arm_rc;
}

int  opendisplay_display_pipe_partial_prepare(void) { return g_partial_prepare_rc; }
void opendisplay_display_abort(void)                { ++g_aborts; g_dw_written = 0u; }
bool opendisplay_display_boot_apply(void)           { return false; }
void opendisplay_display_park_pins(void)            { }
void opendisplay_display_power_off(void)            { }

/* ------------------------------------------------------------------------------ fake kernel --- */

uint32_t fake_k_sleep_ms;
void fake_zephyr_reset(void) { fake_k_sleep_ms = 0u; }
void k_msleep(int32_t ms)    { fake_k_sleep_ms += (uint32_t)((ms > 0) ? ms : 0); }

/* --------------------------------------------------------------------------------- helpers --- */

/* ctx is only ever passed through to the reply seam, which ignores it. */
static const od_cmd_ctx_t CTX = { { OD_ORIGIN_BLE, 1u }, NULL };

#define PIPE_MAX_FRAME_LOCAL 244u

static void reset_all(uint32_t total)
{
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_flushes = 0u;
    g_reply_fail_on = 0u;
    g_reply_calls = 0u;
    g_dw_total = total;
    g_dw_written = 0u;
    g_dw_active = false;
    g_partial_active = false;
    g_aborts = 0u;
    g_etag_clears = 0u;
    g_prepares = 0u;
    g_refreshes = 0u;
    g_data_rc = 0;
    g_prepare_rc = 0;
    g_refresh_rc = 0;
    g_refresh_ok = true;
    g_full_start_rc = 0;
    g_partial_arm_rc = 0;
    g_partial_prepare_rc = 0;
    fake_zephyr_reset();
    opendisplay_pipe_write_reset();
}

/* A well-formed full-image START: version 1, no flags, window 4, ack every 2. */
static od_cmd_result_t start_ok(uint32_t total, uint8_t window, uint8_t ack_every)
{
    struct PipeStartRequest req;

    memset(&req, 0, sizeof req);
    req.version = 1u;
    req.flags = 0u;
    req.req_window = window;
    req.req_ack_every = ack_every;
    req.client_max_frame = PIPE_MAX_FRAME_LOCAL;
    req.total_size = total;
    return opendisplay_pipe_write_start(&CTX, (const uint8_t *)&req, (uint16_t)sizeof req);
}

/* One DATA frame: [seq][payload...]. */
static od_cmd_result_t data_frame(uint8_t seq, uint16_t payload_len)
{
    uint8_t frame[256];

    frame[0] = seq;
    memset(frame + 1, 0xC3, payload_len);
    return opendisplay_pipe_write_data(&CTX, frame, (uint16_t)(payload_len + 1u));
}

static bool last_is(uint8_t status, uint8_t opcode)
{
    return g_sent_n > 0u && g_sent[g_sent_n - 1u].len >= 2u &&
           g_sent[g_sent_n - 1u].data[0] == status && g_sent[g_sent_n - 1u].data[1] == opcode;
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_start_bad_header_nacks(void)
{
    uint8_t stub[2] = { 0x01u, 0x00u };
    struct PipeStartRequest req;

    CASE("a START too short to hold the header is refused, and says so");
    reset_all(1000u);
    CHECK(opendisplay_pipe_write_start(&CTX, stub, sizeof stub) == OD_CMD_NACK);
    CHECK(g_sent_n == 1u);
    CHECK(last_is(RESP_NACK, 0x80u));
    CHECK(g_sent[0].data[2] == OD_ERR_PIPE_START_BAD_HEADER);
    CHECK(g_sent[0].plain);                       /* a hard NACK is never sealed */
    CHECK(!opendisplay_pipe_write_active());

    CASE("a START naming an unknown protocol version is refused");
    reset_all(1000u);
    memset(&req, 0, sizeof req);
    req.version = 0x7Fu;
    req.total_size = 1000u;
    CHECK(opendisplay_pipe_write_start(&CTX, (const uint8_t *)&req, (uint16_t)sizeof req)
          == OD_CMD_NACK);
    CHECK(last_is(RESP_NACK, 0x80u));

    CASE("a START carrying an unknown flag is refused with its own code");
    reset_all(1000u);
    memset(&req, 0, sizeof req);
    req.version = 1u;
    req.flags = 0x40u;
    req.total_size = 1000u;
    CHECK(opendisplay_pipe_write_start(&CTX, (const uint8_t *)&req, (uint16_t)sizeof req)
          == OD_CMD_NACK);
    CHECK(g_sent[0].data[2] == OD_ERR_PIPE_START_UNKNOWN_FLAG);

    CASE("a START whose total does not match the panel is refused");
    reset_all(1000u);
    CHECK(start_ok(999u, 4u, 2u) == OD_CMD_NACK);
    CHECK(g_sent[0].data[2] == OD_ERR_PIPE_START_SIZE_MISMATCH);
    CHECK(!opendisplay_pipe_write_active());
}

static void test_start_ok(void)
{
    CASE("an accepted START answers 0x80 sealed and opens the transfer");
    reset_all(1000u);
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_OK);
    CHECK(g_sent_n == 1u);
    CHECK(last_is(RESP_ACK, 0x80u));
    CHECK(!g_sent[0].plain);
    CHECK(opendisplay_pipe_write_active());
}

static void test_cadence_ack(void)
{
    CASE("in-order DATA is accepted; the SACK arrives on the agreed cadence, not per frame");
    reset_all(1000u);
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_OK);
    g_sent_n = 0u;

    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    CHECK(g_sent_n == 0u);                        /* ack_every == 2: nothing yet */
    CHECK(data_frame(1u, 100u) == OD_CMD_OK);
    CHECK(g_sent_n == 1u);
    CHECK(last_is(RESP_ACK, 0x81u));
    CHECK(!g_sent[0].plain);                      /* PIPE ACKs are application responses */
    CHECK(g_sent[0].len == 7u);
}

static void test_data_outside_window_nacks(void)
{
    CASE("a sequence outside the window in both directions is fatal, and reported as refused");
    reset_all(1000u);
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_OK);
    g_sent_n = 0u;

    CHECK(data_frame(60u, 10u) == OD_CMD_NACK);
    CHECK(last_is(RESP_NACK, 0x81u));
    CHECK(g_sent[g_sent_n - 1u].plain);
    CHECK(g_aborts >= 1u);
}

static void test_data_for_a_dead_transfer_is_silent_but_refused(void)
{
    CASE("DATA with no transfer open draws NOTHING -- a 0x81 NACK would kill a live upload");
    reset_all(1000u);
    CHECK(data_frame(0u, 10u) == OD_CMD_NACK);
    CHECK(g_sent_n == 0u);

    CASE("and DATA after a fatal error is equally silent, and equally a refusal");
    reset_all(1000u);
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_OK);
    CHECK(data_frame(60u, 10u) == OD_CMD_NACK);   /* fatal: sets the error flag */
    g_sent_n = 0u;
    CHECK(data_frame(0u, 10u) == OD_CMD_NACK);
    CHECK(g_sent_n == 0u);
}

static void test_consume_failure_nacks(void)
{
    CASE("a panel that refuses the payload turns the frame into a fatal NACK");
    reset_all(1000u);
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_OK);
    g_sent_n = 0u;
    g_data_rc = -1;
    CHECK(data_frame(0u, 100u) == OD_CMD_NACK);
    CHECK(last_is(RESP_NACK, 0x81u));
}

static void test_gap_sack(void)
{
    CASE("an out-of-order frame inside the window is ACCEPTED and draws an immediate gap SACK");
    reset_all(1000u);
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_OK);
    g_sent_n = 0u;

    CHECK(data_frame(1u, 100u) == OD_CMD_OK);     /* seq 0 is missing */
    CHECK(g_sent_n == 1u);
    CHECK(last_is(RESP_ACK, 0x81u));

    CASE("and the gap closes when the missing frame arrives, draining the queued one");
    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    CHECK(g_dw_written == 200u);                  /* both consumed, in order */
}

static void test_auto_complete(void)
{
    CASE("the DATA frame that meets the total completes the transfer and refreshes");
    reset_all(200u);
    CHECK(start_ok(200u, 4u, 2u) == OD_CMD_OK);
    g_sent_n = 0u;

    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    CHECK(g_refreshes == 0u);
    CHECK(data_frame(1u, 100u) == OD_CMD_OK);     /* now at the total */
    CHECK(g_prepares == 1u);
    CHECK(g_refreshes == 1u);
    /* The ack is on air BEFORE the blocking refresh: without the barrier the host spends its
     * tail-flush read and aborts a transfer that in fact completed. */
    CHECK(g_flushes == 1u);
    CHECK(fake_k_sleep_ms >= 20u);
    CHECK(last_is(RESP_ACK, 0x73u));              /* refresh reported OK */
    CHECK(!opendisplay_pipe_write_active());

    CASE("a refresh that times out is still an ACCEPTED transfer -- 0x74, not a refusal");
    reset_all(200u);
    g_refresh_ok = false;
    CHECK(start_ok(200u, 4u, 2u) == OD_CMD_OK);
    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    CHECK(data_frame(1u, 100u) == OD_CMD_OK);
    CHECK(last_is(RESP_ACK, 0x74u));
}

static void test_explicit_end(void)
{
    CASE("an explicit END on a complete transfer: tail SACK, END ack, then the refresh status");
    reset_all(200u);
    CHECK(start_ok(200u, 4u, 4u) == OD_CMD_OK);   /* ack_every 4: no cadence ack in the way */
    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    g_sent_n = 0u;
    CHECK(data_frame(1u, 99u) == OD_CMD_OK);      /* one byte short of the total */
    g_sent_n = 0u;

    CHECK(opendisplay_pipe_write_end(&CTX, NULL, 0u) == OD_CMD_NACK);   /* incomplete */
    CHECK(last_is(RESP_NACK, 0x82u));

    CASE("and a complete one is accepted");
    reset_all(200u);
    CHECK(start_ok(200u, 4u, 4u) == OD_CMD_OK);
    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    CHECK(data_frame(1u, 100u) == OD_CMD_OK);
    /* Auto-complete already fired at the total, so the transfer is closed. An END after it is a
     * refusal, and that is the shipped behaviour: there is nothing left to end. */
    g_sent_n = 0u;
    CHECK(opendisplay_pipe_write_end(&CTX, NULL, 0u) == OD_CMD_NACK);
    CHECK(last_is(RESP_NACK, 0x82u));

    CASE("an END with no transfer open is refused, plainly");
    reset_all(200u);
    CHECK(opendisplay_pipe_write_end(&CTX, NULL, 0u) == OD_CMD_NACK);
    CHECK(last_is(RESP_NACK, 0x82u));
    CHECK(g_sent[0].plain);
}

/* THE CASE THE VERDICT WAS ADDED FOR. od_reply() substitutes a plaintext hard NACK for a frame it
 * could not seal, and every 0x81 NACK is fatal to the client's upload loop. The transfer must
 * stop, no success may be queued behind the refusal, and the command must report NACK -- if it
 * reported OK the frame would also stamp the session's activity clock. */
static void test_reply_substitution_aborts(void)
{
    CASE("a substituted cadence ACK stops the transfer and is reported as refused");
    reset_all(1000u);
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_OK);
    g_sent_n = 0u;
    g_reply_calls = 0u;
    g_reply_fail_on = 1u;                         /* ack_every is 2, so the first ack is the
                                                     one the second DATA frame triggers */

    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    CHECK(data_frame(1u, 100u) == OD_CMD_NACK);
    CHECK(g_sent_n == 0u);                        /* nothing queued behind the substitution */
    CHECK(g_aborts >= 1u);
    CHECK(!opendisplay_pipe_write_active());

    CASE("a substituted END ack stops before the panel is touched");
    reset_all(200u);
    CHECK(start_ok(200u, 4u, 4u) == OD_CMD_OK);
    CHECK(data_frame(0u, 100u) == OD_CMD_OK);
    g_sent_n = 0u;
    g_reply_calls = 0u;
    g_reply_fail_on = 2u;                         /* 1 = the tail SACK, 2 = the END ack */
    CHECK(data_frame(1u, 100u) == OD_CMD_NACK);
    CHECK(g_refreshes == 0u);                     /* refused on the wire, so nothing is displayed */
    CHECK(g_aborts >= 1u);

    CASE("a substituted START ack is reported as refused rather than opening a transfer");
    reset_all(1000u);
    g_reply_fail_on = 1u;
    CHECK(start_ok(1000u, 4u, 2u) == OD_CMD_NACK);
    CHECK(g_sent_n == 0u);
}

int main(void)
{
    test_start_bad_header_nacks();
    test_start_ok();
    test_cadence_ack();
    test_data_outside_window_nacks();
    test_data_for_a_dead_transfer_is_silent_but_refused();
    test_consume_failure_nacks();
    test_gap_sack();
    test_auto_complete();
    test_explicit_end();
    test_reply_substitution_aborts();

    printf("pipe_write: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
