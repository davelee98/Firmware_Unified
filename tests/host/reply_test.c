/* reply_test.c -- shared/core/od_reply.c: the seal-or-plain decision.
 *
 * The property under test is not "does it encrypt" but "is confidentiality chosen by the CALL".
 * Both targets previously inferred it from the response bytes, and Nordic's version sealed its own
 * rejection frames -- which py-opendisplay then decrypted and validated as an ACK for a command
 * the device had refused. So the cases that matter are the ones where the bytes would mislead.
 */

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

#define SENT_MAX 16u

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

/* ------------------------------------------------------------------- fake od_session_app seam --- */

static struct od_session g_app_session;
static bool     g_security_on;
static uint32_t g_now_ms;
static unsigned g_reports;
static int      g_last_seal_result;

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
    (void)cmd; (void)report;
    ++g_reports;
    if (op == OD_SESSION_APP_SEAL) { g_last_seal_result = result; }
}

/* od_txq's drop seam. Counted so a test can assert that a discarded entry was REPORTED, not just
 * that it vanished -- the difference between a diagnosable failure and a silent one. */
static unsigned g_dropped;
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
    ++g_dropped;
}

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE  = { OD_ORIGIN_BLE, 7u };
static const od_reply_t TLS  = { OD_ORIGIN_LAN_TLS, 7u };

static void setup(bool security_on, bool open_session)
{
    uint8_t server_nonce[16];

    fake_reset();
    sec_init(0);
    memset(&g_app_session, 0, sizeof g_app_session);
    od_session_init(&g_app_session, 0);
    od_txq_reset();
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_tag_live = true;
    g_security_on = security_on;
    g_now_ms = 1000u;
    g_reports = 0u;
    g_last_seal_result = -1;

    if (open_session) {
        CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false)
              == OD_SESSION_AUTH_ESTABLISHED);
    }
}

/* [cmd:2][payload] -- what od_session_seal takes and what a handler produces. */
static const uint8_t FRAME[6] = { 0x00u, 0x71u, 0xDEu, 0xADu, 0xBEu, 0xEFu };

/* ----------------------------------------------------------------------------------- cases --- */

static void test_plain_when_security_is_off(void)
{
    od_tx_reservation_t r;

    CASE("no security configured: the response goes out verbatim");
    setup(false, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == sizeof FRAME);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) == 0);
}

static void test_sealed_when_session_is_live(void)
{
    od_tx_reservation_t r;
    uint8_t plain[OD_SESSION_PLAIN_MAX];
    uint16_t plain_len = 0u;

    CASE("live session: the response is sealed, and round-trips back to the original");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);
    /* Sealing adds exactly 29 bytes to the plain frame. */
    CHECK(g_sent[0].len == sizeof FRAME + 29u);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) != 0);   /* it is NOT the plaintext */
    /* The two command bytes are echoed in the clear; the rest is the envelope. */
    CHECK(g_sent[0].data[0] == 0x00u);
    CHECK(g_sent[0].data[1] == 0x71u);
    CHECK(g_reports >= 1u);
    CHECK(g_last_seal_result == (int)OD_SESSION_SEAL_OK);

    CASE("and what was sealed is exactly what the handler produced");
    CHECK(od_session_open(&g_app_session, 0x0071u,
                          od_span_make(g_sent[0].data + 2, (size_t)(g_sent[0].len - 2u)),
                          plain, sizeof plain, &plain_len, g_now_ms, NULL)
          == OD_SESSION_OPEN_OK);
    CHECK(plain_len == sizeof FRAME - 2u);
    CHECK(memcmp(plain, FRAME + 2, plain_len) == 0);
}

static void test_reply_plain_never_seals(void)
{
    od_tx_reservation_t r;

    CASE("od_reply_plain leaves a control frame alone even with a live session");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply_plain(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == sizeof FRAME);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) == 0);

    CASE("THE REGRESSION THIS EXISTS FOR: a rejection frame stays readable");
    /* {0x00, cmd, 0xFF} has its marker in byte 2 and 0x00 in byte 0. Nordic's old predicate read
     * byte 0, so it sealed this -- and py-opendisplay decrypts any 31+ byte response and returns
     * before its raw[2] guard, so the host validated a REFUSED command as an ACK. Choosing at the
     * call site removes the class, not just this instance. */
    setup(true, true);
    {
        const uint8_t nack[3] = { 0x00u, 0x71u, 0xFFu };
        CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
        CHECK(od_reply_plain(&r, &BLE, nack, sizeof nack) == OD_TXQ_OK);
        CHECK(od_txq_process() == 1u);
        CHECK(g_sent[0].len == 3u);                     /* NOT 32 */
        CHECK(g_sent[0].data[2] == 0xFFu);              /* the host can still see the marker */
    }
}

static void test_tls_lan_is_never_double_wrapped(void)
{
    od_tx_reservation_t r;

    CASE("TLS-LAN goes plain at the application layer even through od_reply");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &TLS, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == sizeof FRAME);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) == 0);
}

static void test_no_session_substitutes_a_nack(void)
{
    od_tx_reservation_t r;

    CASE("security ON but no session: the payload must NOT go out in the clear");
    setup(true, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_SEAL_FAILED);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);
    /* A hard NACK, not the response. Sending the response plaintext would hand an unauthenticated
     * peer exactly the payload the session exists to hide. */
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
    CHECK(g_sent[0].data[1] == 0x71u);            /* the opcode is echoed so a client can match it */
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) != 0);
}

static void test_oversized_becomes_a_nack(void)
{
    od_tx_reservation_t r;
    static uint8_t big[OD_SESSION_PLAIN_FRAME_MAX + 8];

    memset(big, 0x5Au, sizeof big);
    big[0] = 0x00u;
    big[1] = 0x71u;

    CASE("exactly OD_SESSION_PLAIN_FRAME_MAX seals to the 253-byte ceiling");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, big, OD_SESSION_PLAIN_FRAME_MAX) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == OD_SESSION_SEALED_MAX);

    CASE("one byte more is refused and becomes a NACK, with no second reply");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, big, OD_SESSION_PLAIN_FRAME_MAX + 1u) == OD_TXQ_TOO_LARGE);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);                        /* exactly one frame, the NACK */
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
    CHECK(r.remaining == 0u);                     /* the unit went to the NACK */
}

static void test_counter_exhaustion_clears_the_session(void)
{
    od_tx_reservation_t r;

    CASE("a spent TX counter forces re-authentication rather than reusing a nonce");
    setup(true, true);
    g_app_session.tx_counter = UINT64_MAX;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_SEAL_FAILED);
    CHECK(!od_session_authenticated(&g_app_session));   /* cleared, so the client must re-auth */
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
}

static void test_argument_and_accounting(void)
{
    od_tx_reservation_t r;

    CASE("bad arguments spend nothing and queue nothing");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(NULL, &BLE, FRAME, sizeof FRAME) == OD_TXQ_INVARIANT);
    CHECK(od_reply(&r, NULL, FRAME, sizeof FRAME) == OD_TXQ_INVARIANT);
    CHECK(od_reply(&r, &BLE, NULL, 4u) == OD_TXQ_INVARIANT);
    CHECK(od_reply(&r, &BLE, FRAME, 0u) == OD_TXQ_INVARIANT);
    CHECK(r.remaining == 1u);
    CHECK(od_txq_depth() == 0u);

    CASE("a frame too short to carry an opcode emits nothing at all");
    /* Under two bytes there is no opcode to echo, so no honest NACK can be built -- the caller's
     * release() reclaims the unit instead. */
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, 1u) == OD_TXQ_INVARIANT);
    CHECK(od_txq_depth() == 0u);
    CHECK(g_sent_n == 0u);
}

int main(void)
{
    test_plain_when_security_is_off();
    test_sealed_when_session_is_live();
    test_reply_plain_never_seals();
    test_tls_lan_is_never_double_wrapped();
    test_no_session_substitutes_a_nack();
    test_oversized_becomes_a_nack();
    test_counter_exhaustion_clears_the_session();
    test_argument_and_accounting();

    printf("reply: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
