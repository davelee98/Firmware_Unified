/* config_read_test.c -- CONFIG_READ as a resumable producer.
 *
 * The property that matters is NO TRUNCATION AND NO DUPLICATION UNDER BACKPRESSURE. The shipped
 * handler emitted every chunk in one call and drained the ring between them, which only worked
 * because it could block the loop task; with a finite queue that stops being true. So the cases
 * here run the producer against a queue that keeps filling up, and reassemble what came out.
 */

#include "od_config_read.h"

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

#define SENT_MAX 128u
static struct { uint16_t len; uint8_t data[OD_TX_FRAME_MAX]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static bool     g_radio_stalled;      /* RETRY everything, so the queue fills */
static bool     g_tag_dead;           /* the link dies mid-read */

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (g_radio_stalled) { return OD_RADIO_RETRY; }
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
    return !g_tag_dead;
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

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE = { OD_ORIGIN_BLE, 5u };

/* Big enough that its chunk count EXCEEDS the queue depth, which is the only way the backpressure
 * case actually stalls: at ~96 bytes of payload per chunk, OD_TXQ_SLOTS-1 slots absorb roughly
 * 3.1 KB, so a 1.4 KB read simply completes and proves nothing. OD_CONFIG_MAX_SIZE is 4096. */
static uint8_t g_blob[4096];

static void setup(bool security_on)
{
    unsigned i;

    fake_reset();
    sec_init(0);
    memset(&g_app_session, 0, sizeof g_app_session);
    od_session_init(&g_app_session, 0);
    od_config_read_cancel();
    od_txq_reset();
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_radio_stalled = false;
    g_tag_dead = false;
    g_security_on = security_on;
    for (i = 0; i < sizeof g_blob; ++i) {
        g_blob[i] = (uint8_t)(i * 7u + 3u);   /* position-dependent, so a splice is detectable */
    }
}

/* Reassemble the payloads of every captured chunk and compare against the blob. Also checks the
 * header shape of each: chunk numbers ascend by one and only chunk 0 carries the total length. */
static void check_reassembly(uint32_t blob_len)
{
    static uint8_t got[sizeof g_blob];
    uint32_t got_n = 0u;
    unsigned i;

    for (i = 0; i < g_sent_n; ++i) {
        const uint8_t *f = g_sent[i].data;
        uint16_t hdr;

        CHECK(f[0] == RESP_ACK);
        CHECK(f[1] == RESP_CONFIG_READ);
        CHECK((uint16_t)(f[2] | ((uint16_t)f[3] << 8)) == (uint16_t)i);
        if (i == 0u) {
            CHECK((uint32_t)(f[4] | ((uint32_t)f[5] << 8)) == blob_len);
            hdr = 6u;
        } else {
            hdr = 4u;
        }
        CHECK(g_sent[i].len <= MAX_RESPONSE_DATA_SIZE);
        if (got_n + (uint32_t)(g_sent[i].len - hdr) <= sizeof got) {
            memcpy(got + got_n, f + hdr, (size_t)(g_sent[i].len - hdr));
            got_n += (uint32_t)(g_sent[i].len - hdr);
        }
    }
    CHECK(got_n == blob_len);
    CHECK(memcmp(got, g_blob, blob_len) == 0);
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_full_read_unimpeded(void)
{
    od_tx_reservation_t r;
    const uint32_t len = 900u;
    unsigned passes = 0u;

    CASE("a read with capacity available completes and reassembles exactly");
    setup(false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, len) == OD_TXQ_OK);
    while (od_config_read_active() && passes < 200u) {
        (void)od_txq_process();               /* the loop drains between passes */
        CHECK(od_config_read_pump() == OD_TXQ_OK);
        ++passes;
    }
    (void)od_txq_process();
    CHECK(!od_config_read_active());
    check_reassembly(len);

    CASE("chunk 0 carries the total length and later chunks do not");
    /* 900 bytes: chunk 0 takes 94, the rest take 96 -> 1 + ceil(806/96) = 10 chunks. */
    CHECK(g_sent_n == 10u);
    CHECK(g_sent[0].len == MAX_RESPONSE_DATA_SIZE);
    CHECK(g_sent[1].len == MAX_RESPONSE_DATA_SIZE);
}

static void test_backpressure_never_truncates(void)
{
    od_tx_reservation_t r;
    const uint32_t len = 4000u;
    unsigned passes = 0u;

    CASE("a stalled radio fills the queue; the producer waits and loses nothing");
    setup(false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, len) == OD_TXQ_OK);

    /* Stall the radio until the queue is completely full and pump() can only return FULL. */
    g_radio_stalled = true;
    while (od_config_read_active() && od_config_read_pump() == OD_TXQ_OK) { }
    CHECK(od_config_read_active());            /* still pending, not failed and not truncated */
    CHECK(od_config_read_pump() == OD_TXQ_FULL);
    CHECK(od_config_read_pump() == OD_TXQ_FULL);   /* repeatedly FULL is not an error */
    CHECK(od_config_read_active());

    /* Now let it drain, alternating one drain pass with one produce pass -- the shape a real loop
     * has, and the one where an off-by-one drops or repeats a chunk. */
    g_radio_stalled = false;
    while ((od_config_read_active() || od_txq_depth() > 0u) && passes < 2000u) {
        (void)od_txq_process();
        (void)od_config_read_pump();
        ++passes;
    }
    CHECK(!od_config_read_active());
    CHECK(od_txq_depth() == 0u);
    check_reassembly(len);
    CHECK(od_txq_reserved() == 0u);             /* no capacity leaked across all that */
}

static void test_load_failure(void)
{
    od_tx_reservation_t r;

    CASE("a load failure is a completed read with one 4-byte error frame");
    setup(false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, NULL, 0u) == OD_TXQ_OK);
    CHECK(!od_config_read_active());            /* NOT pending -- nothing to resume */
    (void)od_txq_process();
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == 4u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
    CHECK(g_sent[0].data[1] == RESP_CONFIG_READ);

    CASE("and it stays PLAINTEXT with a live session -- a hard NACK must remain readable");
    /* Sealed, a client whose session had just died could not read why its read failed. Section 3.6
     * puts every hard NACK on the explicit plaintext path; this one was on od_reply until an
     * independent review caught it. */
    setup(true);
    {
        uint8_t server_nonce[16];
        od_tx_reservation_t r2;
        CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false)
              == OD_SESSION_AUTH_ESTABLISHED);
        CHECK(od_txq_reserve(1u, &r2) == OD_TXQ_OK);
        CHECK(od_config_read_start(&BLE, &r2, NULL, 0u) == OD_TXQ_OK);
        (void)od_txq_process();
        CHECK(g_sent_n == 1u);
        CHECK(g_sent[0].len == 4u);                  /* NOT 33 */
        CHECK(g_sent[0].data[0] == RESP_NACK);
    }
}

static void test_second_read_is_refused(void)
{
    od_tx_reservation_t r1, r2;

    CASE("starting a second read while one is active is an invariant failure");
    /* The dispatcher is supposed to have deferred it. Restarting would break the chunk count
     * already promised to the host in chunk 0, and could splice two configs into one read-back. */
    setup(false);
    CHECK(od_txq_reserve(1u, &r1) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r1, g_blob, 900u) == OD_TXQ_OK);
    CHECK(od_config_read_active());
    CHECK(od_txq_reserve(1u, &r2) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r2, g_blob, 900u) == OD_TXQ_INVARIANT);
    CHECK(od_config_read_active());             /* the FIRST read is untouched */
    od_txq_release(&r2);
}

static void test_cancel_releases_capacity(void)
{
    od_tx_reservation_t r;

    CASE("cancel ends the read and leaks no reservation");
    setup(false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, 4000u) == OD_TXQ_OK);
    CHECK(od_config_read_active());
    od_config_read_cancel();
    CHECK(!od_config_read_active());
    /* Everything the producer held is back, so a full-size reserve succeeds. */
    (void)od_txq_process();
    {
        od_tx_reservation_t big;
        CHECK(od_txq_reserve((uint8_t)(OD_TXQ_SLOTS - 1u), &big) == OD_TXQ_OK);
        od_txq_release(&big);
    }
    CHECK(od_txq_reserved() == 0u);

    CASE("cancel is idempotent and safe when idle");
    od_config_read_cancel();
    od_config_read_cancel();
    CHECK(!od_config_read_active());
}

static void test_chunks_are_sealed_when_a_session_is_live(void)
{
    od_tx_reservation_t r;
    uint8_t server_nonce[16];
    unsigned passes = 0u;

    CASE("config data is application payload, so a live session seals every chunk");
    setup(true);
    CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, 300u) == OD_TXQ_OK);
    while (od_config_read_active() && passes < 100u) {
        (void)od_txq_process();
        (void)od_config_read_pump();
        ++passes;
    }
    (void)od_txq_process();
    CHECK(g_sent_n > 0u);
    /* Sealing adds 29 bytes, and the two leading bytes are echoed in the clear. */
    CHECK(g_sent[0].len == MAX_RESPONSE_DATA_SIZE + 29u);
    CHECK(g_sent[0].data[0] == RESP_ACK);
    CHECK(g_sent[0].data[1] == RESP_CONFIG_READ);
    /* And the chunk number is NOT readable in the clear -- it is inside the envelope. */
    CHECK(!(g_sent[0].data[2] == 0x00u && g_sent[0].data[3] == 0x00u &&
            g_sent[0].data[4] == (uint8_t)(300u & 0xFFu)));
}

static void test_a_seal_failure_ends_the_read(void)
{
    od_tx_reservation_t r;
    uint8_t server_nonce[16];
    unsigned passes = 0u;
    unsigned nacks = 0u;
    unsigned i;

    /* THE MECHANISM THAT ACTUALLY BIT, and it is not the dead-tag one. A session that expires
     * mid-read leaves the producer emitting into od_reply with security still enabled and no
     * session: it substitutes a plaintext hard NACK and reports SEAL_FAILED. Treated as retryable,
     * that is an unbounded NACK flood -- one per loop pass, forever -- while od_dispatch defers
     * every config read and write behind the read that will never finish.
     *
     * A dead tag cannot show this: od_reply returns GONE without ever queueing a frame. */
    CASE("a session that dies mid-read ends the read instead of flooding NACKs");
    setup(true);
    CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false) == OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, 4096u) == OD_TXQ_OK);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u);                       /* chunk 0 went out sealed */
    CHECK(od_config_read_active());

    od_session_clear(&g_app_session);            /* the session expires mid-read */
    CHECK(od_config_read_pump() == OD_TXQ_SEAL_FAILED);
    CHECK(!od_config_read_active());             /* terminal, not pending */

    CASE("and the host gets exactly ONE hard NACK, not one per pass");
    for (passes = 0u; passes < 50u; ++passes) {
        (void)od_txq_process();
        (void)od_config_read_pump();             /* idle: must stay silent */
    }
    (void)od_txq_process();
    for (i = 0u; i < g_sent_n; ++i) {
        if (g_sent[i].len >= 2u && g_sent[i].data[0] == RESP_NACK &&
            g_sent[i].data[1] == RESP_CONFIG_READ) {
            ++nacks;
        }
    }
    CHECK(nacks == 1u);

    CASE("and nothing is left reserved");
    CHECK(od_txq_reserved() == 0u);
}

/* A chunk that could not be QUEUED must not advance the read. Reserve-then-emit means the usual
 * failure is FULL, which returns before the frame is built -- so the only way to exercise
 * emit_chunk's own failure path is to make the reply itself fail, which a dead link does. Without
 * this case, advancing the offset on a failed reply is invisible: the read simply completes with a
 * hole in it, and the host reassembles a config that passes CRC and is wrong.
 */
static void test_a_terminal_failure_ends_the_read(void)
{
    od_tx_reservation_t r;
    const uint32_t len = 900u;

    /* THE RETRYABLE CASE IS FULL, AND ONLY FULL -- test_backpressure_never_truncates covers it:
     * the producer stays pending, loses nothing, and resumes at the same chunk.
     *
     * Everything else is terminal, and an earlier version of this case asserted the opposite: it
     * killed the tag, checked the read was still active, then "revived" the same tag and watched
     * the read finish. That scenario is not reachable. An owner word is an INSTANCE identity, so a
     * peer that comes back comes back as a different word; the one this read is bound to is gone
     * for good. Pinning the resume made the wedge look like correct behaviour.
     *
     * What the wedge costs: the producer stays active forever, so od_dispatch DEFERS every later
     * config read and write for the life of the boot -- and on a seal failure it also queues a
     * fresh NACK every single pass. */
    CASE("a read whose tag dies is OVER, not pending");
    setup(false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, len) == OD_TXQ_OK);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u);                       /* chunk 0 is out */

    g_tag_dead = true;
    CHECK(od_config_read_pump() != OD_TXQ_OK);
    CHECK(!od_config_read_active());             /* terminal: the producer released itself */

    CASE("and it leaves nothing reserved, so the next command is not starved");
    CHECK(od_txq_reserved() == 0u);

    CASE("a later read is accepted normally rather than deferred behind the dead one");
    g_tag_dead = false;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, len) == OD_TXQ_OK);
    CHECK(od_config_read_active());
    od_config_read_cancel();
}

int main(void)
{
    test_full_read_unimpeded();
    test_backpressure_never_truncates();
    test_a_seal_failure_ends_the_read();
    test_a_terminal_failure_ends_the_read();
    test_load_failure();
    test_second_read_is_refused();
    test_cancel_releases_capacity();
    test_chunks_are_sealed_when_a_session_is_live();

    printf("config_read: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
