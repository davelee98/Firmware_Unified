/* core_reset_test.c -- od_core_reset(), the shared half of a teardown.
 *
 * WHAT IT MUST NOT DO carries as much weight as what it does. RX is not in it: Nordic's producer
 * is the BT thread and can push concurrently, and ESP32's connection policy has stricter ordering
 * around its own ring -- so a reset here would race one target and reorder the other. The call
 * sites drain RX themselves, on their own terms.
 *
 * WHAT IS NOT ASSERTED HERE, stated so a green run is not over-read: the ORDER of the three calls.
 * They run straight-line on the consumer context with nothing interleaved, so no test driving the
 * public API can distinguish one order from another -- swapping the cancel and the reset leaves
 * every assertion below unchanged, verified by mutation. The order is not load-bearing (od_core.c
 * says why); what these cases pin is that all three happen, which is.
 */

#include "od_core.h"

#include "od_config_read.h"
#include "od_reply.h"
#include "od_rxq.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_txq.h"
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

static unsigned g_sent_n;
static bool     g_radio_accepts = true;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag; (void)frame; (void)len;
    if (!g_radio_accepts) {
        return OD_RADIO_RETRY;          /* hold entries in the queue so a reset has work to do */
    }
    ++g_sent_n;
    return OD_RADIO_SENT;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return true;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

void od_rxq_app_report(od_rxq_event_t ev, const uint8_t *frame, uint16_t len, uint8_t depth)
{
    (void)ev; (void)frame; (void)len; (void)depth;
}

/* ------------------------------------------------------------------------- fake app session --- */

static struct od_session g_app_session;
static bool     g_security_on = true;
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

static const od_reply_t BLE = { OD_ORIGIN_BLE, 9u };
static uint8_t g_blob[2048];

static void setup(void)
{
    uint8_t server_nonce[16];

    fake_reset();
    sec_init(0);
    memset(&g_app_session, 0, sizeof g_app_session);
    od_session_init(&g_app_session, 0);
    od_config_read_cancel();
    od_txq_reset();
    g_sent_n = 0u;
    g_radio_accepts = true;
    memset(g_blob, 0x5Au, sizeof g_blob);

    CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false)
          == OD_SESSION_AUTH_ESTABLISHED);
}

/* Start a config read and leave it mid-flight with entries stuck in the queue. */
static void start_a_stuck_read(void)
{
    od_tx_reservation_t r;

    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g_radio_accepts = false;                    /* nothing drains: the producer stalls */
    CHECK(od_config_read_start(&BLE, &r, g_blob, sizeof g_blob) == OD_TXQ_OK);
    (void)od_config_read_pump();
    CHECK(od_config_read_active());
    CHECK(od_txq_depth() > 0u);
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_reset_clears_all_three(void)
{
    CASE("a reset cancels the producer, empties the queue and clears the session");
    setup();
    start_a_stuck_read();
    CHECK(od_session_authenticated(&g_app_session));

    od_core_reset();

    CHECK(!od_config_read_active());
    CHECK(od_txq_depth() == 0u);
    CHECK(od_txq_reserved() == 0u);
    CHECK(!od_session_authenticated(&g_app_session));
}

static void test_reset_releases_the_key_slot(void)
{
    CASE("the session goes through od_session_clear(), so its HAL key slot is released");
    setup();
    CHECK(g_key_set_calls == 1u);
    {
        const unsigned clears_before = g_key_clear_calls;
        od_core_reset();
        /* A memset would have left the prepared key in the pool with nothing naming it. This is
         * the assertion that says the reset is a teardown rather than a zeroing. */
        CHECK(g_key_clear_calls == clears_before + 1u);
    }

    CASE("and the slot index survives, so the next handshake reuses it");
    {
        uint8_t server_nonce[16];
        CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false)
              == OD_SESSION_AUTH_ESTABLISHED);
        CHECK(g_app_session.slot == 0u);
    }
}

static void test_reset_does_not_touch_rx(void)
{
    uint8_t frame[4] = { 0x00u, 0x77u, 0x01u, 0x02u };

    CASE("RX is NOT the shared reset's -- its producer differs per target");
    setup();
    (void)od_rxq_reset();
    CHECK(od_rxq_push(frame, sizeof frame, 9u));
    CHECK(od_rxq_peek() != NULL);

    od_core_reset();

    CHECK(od_rxq_peek() != NULL);              /* still there, for the caller to drain */
    (void)od_rxq_reset();
}

static void test_reset_is_idempotent(void)
{
    CASE("resetting twice is the same as resetting once");
    setup();
    start_a_stuck_read();

    od_core_reset();
    od_core_reset();

    CHECK(!od_config_read_active());
    CHECK(od_txq_depth() == 0u);
    CHECK(od_txq_reserved() == 0u);
    CHECK(!od_session_authenticated(&g_app_session));
}

/* A CANCELLED PRODUCER STAYS CANCELLED. Not an ordering check -- see the header comment -- but the
 * property that matters at the call sites: after a disconnect teardown, pumping the producer again
 * (which both targets do, every pass) must not resurrect a chunk of a read the departed client
 * started. Without the cancel, this pump emits chunk N to whoever inherits the link. */
static void test_no_chunk_survives_the_reset(void)
{
    CASE("a cancelled producer emits nothing when the pump runs again");
    setup();
    start_a_stuck_read();

    od_core_reset();
    g_radio_accepts = true;
    g_sent_n = 0u;

    (void)od_config_read_pump();
    (void)od_txq_process();
    CHECK(g_sent_n == 0u);
    CHECK(od_txq_depth() == 0u);
}

int main(void)
{
    test_reset_clears_all_three();
    test_reset_releases_the_key_slot();
    test_reset_does_not_touch_rx();
    test_reset_is_idempotent();
    test_no_chunk_survives_the_reset();

    printf("core_reset: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
