/* dispatch_test.c -- the ordering in od_dispatch.h is the specification; these are its cases.
 *
 * The bugs this guards against are all ORDERING bugs, and each has a specific consequence:
 *   reserve after the gate      -> the gate's own FE/FF answer has no slot and vanishes
 *   reserve after the handler   -> a handler mutates state and then cannot say so, which to the
 *                                  host is indistinguishable from the command never running
 *   defer after decrypt         -> the replay window advanced, so the re-dispatched frame is
 *                                  refused the second time and the transfer stalls
 *   producer conflict too late  -> a config write lands between two read chunks and splices two
 *                                  configs into one CRC-valid read-back
 */

#include "od_dispatch.h"

#include "od_config_read.h"
#include "od_reply.h"
#include "od_session.h"
#include "od_session_app.h"
#include "session_fake.h"

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

/* ------------------------------------------------------------------------------ fake radio --- */

#define SENT_MAX 64u
static struct { uint16_t len; uint8_t data[OD_TX_FRAME_MAX]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static bool     g_tag_live;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].len = len;
        memcpy(g_sent[g_sent_n].data, frame, len);
        ++g_sent_n;
    }
    return OD_RADIO_SENT;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return g_tag_live;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

/* ------------------------------------------------------------------ fake od_session_app seam --- */

static struct od_session g_app_session;
static bool     g_security_on;
static uint32_t g_now_ms = 1000u;

struct od_session *od_session_app_state(void) { return &g_app_session; }
const struct SecurityConfig *od_session_app_security(void)
{
    return g_security_on ? &g_sec : NULL;
}
uint32_t od_session_app_now_ms(void) { return g_now_ms; }
void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
    memcpy(out, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN);
}
void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report)
{
    (void)op; (void)result; (void)cmd; (void)report;
}

/* --------------------------------------------------------------------------- fake handlers --- */

static unsigned        g_handler_calls;
static uint16_t        g_handler_cmd;
/* Wide enough for the largest UNENCRYPTED body the dispatcher can pass (its BLE ceiling minus the
 * two opcode bytes), not just the largest decrypted one. Sized to OD_SESSION_PLAIN_MAX, a mutation
 * that raises the ceiling overflowed this and corrupted the harness -- the defect was detected,
 * but by memory corruption rather than by an assertion. */
static uint8_t         g_handler_body[OD_TX_FRAME_MAX];
static uint16_t        g_handler_body_len;
static od_cmd_result_t g_handler_result;
static uint8_t         g_handler_replies;   /* how many frames the fake handler emits */

od_cmd_result_t od_cmd_dispatch(const od_reply_t *rp, od_tx_reservation_t *r,
                                uint16_t cmd, od_span_t body)
{
    uint8_t k;

    ++g_handler_calls;
    g_handler_cmd = cmd;
    g_handler_body_len = (uint16_t)body.n;
    if (body.n > 0u && body.p != NULL) {
        const size_t n = (body.n < sizeof g_handler_body) ? body.n : sizeof g_handler_body;
        memcpy(g_handler_body, body.p, n);
    }
    for (k = 0; k < g_handler_replies; ++k) {
        uint8_t frame[4];
        frame[0] = RESP_ACK;
        frame[1] = (uint8_t)(cmd & 0xFFu);
        frame[2] = k;
        frame[3] = 0x00u;
        (void)od_reply(r, rp, frame, sizeof frame);
    }
    return g_handler_result;
}

static bool g_mutates_config;
bool od_cmd_mutates_config(uint16_t cmd)
{
    (void)cmd;
    return g_mutates_config;
}

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE = { OD_ORIGIN_BLE, 9u };
static uint8_t g_blob[1024];

static void setup(bool security_on, bool open_session)
{
    uint8_t server_nonce[16];

    fake_reset();
    sec_init(0);
    memset(&g_app_session, 0, sizeof g_app_session);
    od_session_init(&g_app_session, 0);
    od_config_read_cancel();
    od_txq_reset();
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_tag_live = true;
    g_security_on = security_on;
    g_handler_calls = 0u;
    g_handler_cmd = 0u;
    g_handler_body_len = 0u;
    g_handler_result = OD_CMD_OK;
    g_handler_replies = 1u;
    g_mutates_config = false;
    memset(g_blob, 0x5Au, sizeof g_blob);

    if (open_session) {
        CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false)
              == OD_SESSION_AUTH_ESTABLISHED);
    }
}

/* Build [cmd:2][sealed envelope] as the wire carries it. */
static uint16_t make_encrypted(uint16_t cmd, const uint8_t *payload, uint16_t n, uint8_t *out)
{
    uint8_t plain[OD_SESSION_PLAIN_FRAME_MAX];
    uint16_t sealed_len = 0u;

    plain[0] = (uint8_t)((cmd >> 8) & 0xFFu);
    plain[1] = (uint8_t)(cmd & 0xFFu);
    memcpy(plain + 2, payload, n);
    if (od_session_seal(&g_app_session, od_span_make(plain, (uint16_t)(n + 2u)),
                        out, OD_SESSION_SEALED_MAX, &sealed_len, g_now_ms, NULL)
        != OD_SESSION_SEAL_OK) {
        return 0u;
    }
    return sealed_len;
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_plaintext_path(void)
{
    uint8_t frame[6] = { 0x00u, 0x77u, 1u, 2u, 3u, 4u };

    CASE("security off: the body reaches the handler unchanged");
    setup(false, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_cmd == 0x0077u);
    CHECK(g_handler_body_len == 4u);
    CHECK(memcmp(g_handler_body, frame + 2, 4u) == 0);
    CHECK(od_txq_reserved() == 0u);        /* the reservation was released either way */
}

static void test_encrypted_path(void)
{
    uint8_t wire[OD_SESSION_SEALED_MAX];
    const uint8_t payload[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    uint16_t n;

    CASE("security on: the handler sees decrypted plaintext");
    setup(true, true);
    n = make_encrypted(0x0077u, payload, sizeof payload, wire);
    CHECK(n > 0u);
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_body_len == sizeof payload);
    CHECK(memcmp(g_handler_body, payload, sizeof payload) == 0);

    CASE("security on with no session: AUTH_REQUIRED, and the handler never runs");
    setup(true, false);
    {
        uint8_t junk[40];
        memset(junk, 0xAAu, sizeof junk);
        junk[0] = 0x00u; junk[1] = 0x77u;
        CHECK(od_dispatch_frame(&BLE, od_span_make(junk, sizeof junk)) == OD_FRAME_AUTH_REQUIRED);
        CHECK(g_handler_calls == 0u);
    }
}

static void test_structural_and_liveness(void)
{
    uint8_t one[1] = { 0x00u };
    static uint8_t big[OD_BLE_MAX_FRAME + 8];
    uint8_t frame[4] = { 0x00u, 0x77u, 1u, 2u };

    memset(big, 0x11u, sizeof big);
    big[0] = 0x00u; big[1] = 0x77u;

    CASE("a frame with no opcode is rejected and answers nothing");
    setup(false, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(one, 1u)) == OD_FRAME_REJECTED_FRAME);
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent_n == 0u);                 /* nothing to echo, so nothing is said */

    CASE("a BLE frame above 244 is refused by the DISPATCHER, with a NACK the host can see");
    setup(false, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(big, 245u)) == OD_FRAME_REJECTED_FRAME);
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].data[2] == RESP_NACK);

    CASE("a dead tag is STALE_TAG and answers nothing");
    setup(false, false);
    g_tag_live = false;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_STALE_TAG);
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent_n == 0u);
}

static void test_reserve_precedes_the_handler(void)
{
    od_tx_reservation_t hog;
    uint8_t frame[4] = { 0x00u, 0x77u, 1u, 2u };

    CASE("a full queue DEFERS and the handler never runs");
    /* This is the ordering property: a handler that mutates state and then cannot answer looks to
     * the host exactly like a command that never ran. DEFERRED, not refused -- the frame is good
     * and must be re-offered unchanged. */
    setup(false, false);
    CHECK(od_txq_reserve((uint8_t)(OD_TXQ_SLOTS - 1u), &hog) == OD_TXQ_OK);
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_DEFERRED);
    CHECK(g_handler_calls == 0u);
    od_txq_release(&hog);

    CASE("and once capacity returns the same frame dispatches normally");
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
}

static void test_budget_covers_the_worst_case(void)
{
    uint8_t pipe[4] = { 0x00u, 0x81u, 1u, 2u };
    uint8_t other[4] = { 0x00u, 0x77u, 1u, 2u };

    CASE("0x81 reserves three, so a handler emitting three replies is fully answered");
    setup(false, false);
    g_handler_replies = 3u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(pipe, sizeof pipe)) == OD_FRAME_ACCEPTED);
    (void)od_txq_process();
    CHECK(g_sent_n == 3u);                 /* none of them dropped for want of a slot */

    CASE("an ordinary opcode reserves one, and a second reply is refused rather than borrowed");
    setup(false, false);
    g_handler_replies = 2u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(other, sizeof other)) == OD_FRAME_ACCEPTED);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u);                 /* the overrun did not steal another dispatch's unit */
}

static void test_producer_conflict_defers_before_decrypt(void)
{
    od_tx_reservation_t r;
    uint8_t wire[OD_SESSION_SEALED_MAX];
    const uint8_t payload[2] = { 1u, 2u };
    uint16_t n;
    uint64_t rx_before;

    CASE("a config write during an active read is DEFERRED, and the replay window does NOT move");
    /* Deferring after decrypt would advance rx_last, so the re-dispatched frame is refused the
     * second time and the transfer stalls -- the reason this check is above the gate. */
    setup(true, true);
    g_mutates_config = true;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, sizeof g_blob) == OD_TXQ_OK);
    CHECK(od_config_read_active());

    n = make_encrypted(0x0041u, payload, sizeof payload, wire);
    CHECK(n > 0u);
    rx_before = g_app_session.rx_last;
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_DEFERRED);
    CHECK(g_handler_calls == 0u);
    CHECK(g_app_session.rx_last == rx_before);      /* untouched: it never reached the cipher */

    CASE("and the SAME frame dispatches once the read completes");
    od_config_read_cancel();
    g_mutates_config = true;
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);

    CASE("a second CONFIG_READ during an active read is also deferred");
    setup(false, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, sizeof g_blob) == OD_TXQ_OK);
    {
        uint8_t rd[2];
        rd[0] = (uint8_t)((CMD_CONFIG_READ >> 8) & 0xFFu);
        rd[1] = (uint8_t)(CMD_CONFIG_READ & 0xFFu);
        CHECK(od_dispatch_frame(&BLE, od_span_make(rd, sizeof rd)) == OD_FRAME_DEFERRED);
        CHECK(g_handler_calls == 0u);
    }

    CASE("but an UNRELATED command is not blocked by a live read");
    {
        uint8_t other[4] = { 0x00u, 0x77u, 1u, 2u };
        CHECK(od_dispatch_frame(&BLE, od_span_make(other, sizeof other)) == OD_FRAME_ACCEPTED);
        CHECK(g_handler_calls == 1u);
    }
}

static void test_control_opcodes(void)
{
    uint8_t step1[3] = { 0x00u, 0x50u, 0x00u };
    uint8_t ver[2];

    ver[0] = (uint8_t)((CMD_FIRMWARE_VERSION >> 8) & 0xFFu);
    ver[1] = (uint8_t)(CMD_FIRMWARE_VERSION & 0xFFu);

    CASE("AUTHENTICATE is answered by the gate and never reaches a handler");
    setup(true, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(step1, sizeof step1)) == OD_FRAME_AUTH_CONTROL);
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent[0].len == OD_SESSION_STEP1_REPLY_LEN);

    CASE("FIRMWARE_VERSION bypasses the session gate even with security on and no session");
    /* A client must be able to learn what it is talking to before it can authenticate, and a
     * device whose key the host lost would otherwise be unidentifiable. */
    setup(true, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(ver, sizeof ver)) == OD_FRAME_DISCOVERY);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_cmd == CMD_FIRMWARE_VERSION);

    CASE("DISCOVERY is distinct from ACCEPTED, so a version poll cannot stamp activity");
    CHECK(OD_FRAME_DISCOVERY != OD_FRAME_ACCEPTED);
}

static void test_handler_results_map(void)
{
    uint8_t frame[4] = { 0x00u, 0x77u, 1u, 2u };

    CASE("a handler NACK is HANDLER_NACK, which still counts as activity");
    setup(false, false);
    g_handler_result = OD_CMD_NACK;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_HANDLER_NACK);

    CASE("a handler auth rejection is AUTH_REQUIRED, not a NACK");
    /* Without the distinction a TLS client's refused CONFIG_WRITE stamps activity and holds the
     * exclusive link forever. */
    setup(false, false);
    g_handler_result = OD_CMD_AUTH_REJECTED;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_AUTH_REQUIRED);
}

int main(void)
{
    test_plaintext_path();
    test_encrypted_path();
    test_structural_and_liveness();
    test_reserve_precedes_the_handler();
    test_budget_covers_the_worst_case();
    test_producer_conflict_defers_before_decrypt();
    test_control_opcodes();
    test_handler_results_map();

    printf("dispatch: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
