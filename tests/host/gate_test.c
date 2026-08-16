/* gate_test.c -- one case per od_session result, checking the wire bytes AND the outcome.
 *
 * The compiler already guarantees the switches are exhaustive (od_gate.h explains the ratchet, and
 * it was verified by deleting a case and watching the build fail). What it cannot check is that
 * each member maps to the RIGHT wire action, which is where the security consequences live: a
 * refusal answered 0xFE advances the link's auth-abuse run, one answered 0xFF does not, and one
 * answered with silence keeps an upload alive that a NACK would kill.
 */

#include "od_gate.h"

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

#define SENT_MAX 8u
static struct { uint16_t len; uint8_t data[OD_TX_FRAME_MAX]; } g_sent[SENT_MAX];
static unsigned g_sent_n;

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
    return true;
}

/* ------------------------------------------------------------------ fake od_session_app seam --- */

static struct od_session g_app_session;
static uint32_t g_now_ms;
static bool     g_security_on;
static int      g_last_open_result;
static unsigned g_open_reports;

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
    if (op == OD_SESSION_APP_OPEN) { g_last_open_result = result; ++g_open_reports; }
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

static const od_reply_t BLE = { OD_ORIGIN_BLE, 3u };
static uint8_t g_scratch[OD_SESSION_PLAIN_MAX];

static void setup(bool open_session)
{
    uint8_t server_nonce[16];

    fake_reset();
    sec_init(0);
    memset(&g_app_session, 0, sizeof g_app_session);
    od_session_init(&g_app_session, 0);
    od_txq_reset();
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_now_ms = 1000u;
    g_security_on = true;
    g_last_open_result = -1;
    g_open_reports = 0u;
    if (open_session) {
        CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false)
              == OD_SESSION_AUTH_ESTABLISHED);
    }
}

/* Seal a frame through the live session so the gate has something valid to open. Returns the
 * envelope (the bytes AFTER the two command bytes) in `env`. */
static uint16_t seal_frame(const uint8_t *frame, uint16_t len, uint8_t *env, size_t env_cap)
{
    uint8_t sealed[OD_SESSION_SEALED_MAX];
    uint16_t sealed_len = 0u;

    if (od_session_seal(&g_app_session, od_span_make(frame, len), sealed, sizeof sealed,
                        &sealed_len, g_now_ms, NULL) != OD_SESSION_SEAL_OK) {
        return 0u;
    }
    if ((size_t)(sealed_len - 2u) > env_cap) { return 0u; }
    memcpy(env, sealed + 2, (size_t)(sealed_len - 2u));
    return (uint16_t)(sealed_len - 2u);
}

static void drain(void) { (void)od_txq_process(); }

/* --------------------------------------------------------------------------------- inbound --- */

static void test_open_ok(void)
{
    od_tx_reservation_t r;
    uint8_t env[OD_SESSION_ENVELOPE_MAX];
    const uint8_t frame[6] = { 0x00u, 0x71u, 1u, 2u, 3u, 4u };
    uint16_t env_len;
    od_gate_result_t g;

    CASE("OK: the decrypted payload is returned and nothing is queued");
    setup(true);
    env_len = seal_frame(frame, sizeof frame, env, sizeof env);
    CHECK(env_len > 0u);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(env, env_len), g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_ACCEPTED);
    CHECK(g.body.n == sizeof frame - 2u);
    CHECK(memcmp(g.body.p, frame + 2, g.body.n) == 0);
    CHECK(od_txq_depth() == 0u);              /* the handler replies, not the gate */
    CHECK(r.remaining == 1u);                 /* and the unit is still the handler's to spend */
}

static void test_open_no_session(void)
{
    od_tx_reservation_t r;
    uint8_t env[64];
    od_gate_result_t g;

    CASE("NO_SESSION is answered 0xFE and is AUTH_REQUIRED, not CRYPTO_FAILED");
    setup(false);                             /* security on, never authenticated */
    memset(env, 0xAAu, sizeof env);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(env, sizeof env), g_scratch, sizeof g_scratch);
    /* The distinction is the whole reason two outcomes exist: only this one advances the link's
     * auth-abuse run, and only this one tells the host to authenticate rather than to retry. */
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g.body.n == 0u);
    drain();
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_ACK);
    CHECK(g_sent[0].data[1] == 0x71u);
    CHECK(g_sent[0].data[2] == RESP_AUTH_REQUIRED);
}

static void test_open_bad_tag(void)
{
    od_tx_reservation_t r;
    uint8_t env[OD_SESSION_ENVELOPE_MAX];
    const uint8_t frame[6] = { 0x00u, 0x71u, 1u, 2u, 3u, 4u };
    uint16_t env_len;
    od_gate_result_t g;

    CASE("a corrupted tag is answered 0xFF, and the body is NOT readable");
    setup(true);
    env_len = seal_frame(frame, sizeof frame, env, sizeof env);
    env[env_len - 1u] ^= 0x01u;               /* flip a tag bit */
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(env, env_len), g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    /* CCM is decrypt-then-verify, so the scratch holds UNVERIFIED plaintext here. An empty body is
     * what stops a caller acting on it -- acting on it is decrypting for the attacker. */
    CHECK(g.body.n == 0u);
    CHECK(g.body.p == NULL);
    drain();
    CHECK(g_sent[0].data[2] == RESP_NACK);
    CHECK(g_last_open_result == (int)OD_SESSION_OPEN_BAD_TAG);
}

static void test_open_short_and_long(void)
{
    od_tx_reservation_t r;
    static uint8_t big[OD_SESSION_ENVELOPE_MAX + 8];
    uint8_t tiny[4] = { 1u, 2u, 3u, 4u };
    od_gate_result_t g;

    memset(big, 0x33u, sizeof big);

    CASE("an under-length envelope is 0xFF, refused before the cipher");
    setup(true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(tiny, sizeof tiny), g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_last_open_result == (int)OD_SESSION_OPEN_SHORT);
    drain();
    CHECK(g_sent[0].data[2] == RESP_NACK);

    CASE("an over-length envelope is 0xFF, also before the cipher");
    setup(true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(big, OD_SESSION_ENVELOPE_MAX + 1u),
                     g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_last_open_result == (int)OD_SESSION_OPEN_TOO_LONG);
}

static void test_pipe_replay_is_silent(void)
{
    od_tx_reservation_t r;
    uint8_t env[OD_SESSION_ENVELOPE_MAX];
    const uint8_t frame[6] = { 0x00u, 0x81u, 1u, 2u, 3u, 4u };
    uint16_t env_len;
    od_gate_result_t g;

    CASE("THE OD-S1 RULE: a replayed PIPE DATA frame draws NO response");
    setup(true);
    env_len = seal_frame(frame, sizeof frame, env, sizeof env);
    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    /* First delivery is accepted... */
    g = od_gate_open(&r, &BLE, 0x0081u, od_span_make(env, env_len), g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_ACCEPTED);
    /* ...the identical bytes again are a replay, and answering them with the fatal 0x81 NACK would
     * kill the whole upload on the first dropped frame. */
    g = od_gate_open(&r, &BLE, 0x0081u, od_span_make(env, env_len), g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_CRYPTO_DROPPED);
    CHECK(g_last_open_result == (int)OD_SESSION_OPEN_REPLAY);
    drain();
    CHECK(g_sent_n == 0u);                    /* silence, not a NACK */

    CASE("but telemetry still fired -- the report precedes the silent return");
    CHECK(g_open_reports == 2u);

    CASE("and the SAME replay on 0x0071 keeps its NACK: the rule is narrow on purpose");
    setup(true);
    {
        /* Sealed AS 0x0071 -- the two command bytes are the AAD, so a frame sealed under one
         * opcode cannot be opened under another. (Sealing 0x0081 here and opening it as 0x0071
         * fails as BAD_TAG, which is the AAD binding doing its job rather than a replay.) */
        const uint8_t dw[6] = { 0x00u, 0x71u, 1u, 2u, 3u, 4u };
        env_len = seal_frame(dw, sizeof dw, env, sizeof env);
    }
    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(env, env_len), g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_ACCEPTED);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(env, env_len), g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    drain();
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].data[2] == RESP_NACK);
}

static void test_bad_scratch(void)
{
    od_tx_reservation_t r;
    uint8_t env[64];
    uint8_t small[8];
    od_gate_result_t g;

    CASE("a scratch too small to hold a decrypted frame is refused, not written into");
    setup(true);
    memset(env, 0xAAu, sizeof env);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(env, sizeof env), small, sizeof small);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g.body.n == 0u);
    CHECK(g_open_reports == 0u);              /* the session was never even asked */

    CASE("a NULL scratch likewise");
    setup(true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_open(&r, &BLE, 0x0071u, od_span_make(env, sizeof env), NULL, OD_SESSION_PLAIN_MAX);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
}

/* ---------------------------------------------------------------------------- the handshake --- */

static void test_authenticate_dispositions(void)
{
    od_tx_reservation_t r;
    uint8_t step1[1] = { 0x00u };
    uint8_t junk[7] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u };
    od_frame_outcome_t o;

    CASE("step 1 is AUTH_CONTROL and its 23-byte reply goes out PLAINTEXT");
    setup(false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    o = od_gate_authenticate(&r, &BLE, od_span_make(step1, 1u));
    CHECK(o == OD_FRAME_AUTH_CONTROL);
    drain();
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == OD_SESSION_STEP1_REPLY_LEN);
    CHECK(g_sent[0].data[0] == RESP_ACK);
    CHECK(g_sent[0].data[1] == RESP_AUTHENTICATE);
    CHECK(g_sent[0].data[2] == AUTH_STATUS_CHALLENGE);

    CASE("a malformed body is AUTH_CONTROL with the core's own 3-byte error reply");
    setup(false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    o = od_gate_authenticate(&r, &BLE, od_span_make(junk, sizeof junk));
    CHECK(o == OD_FRAME_AUTH_CONTROL);
    drain();
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[2] == AUTH_STATUS_ERROR);

    CASE("a successful step 2 is AUTH_ESTABLISHED, not merely AUTH_CONTROL");
    /* The two differ in policy: only this one resets the auth-abuse run, and NEITHER stamps
     * activity -- a peer that has just authenticated has not yet done work worth holding the
     * exclusive link for. */
    setup(false);
    {
        uint8_t rsp[OD_SESSION_REPLY_MAX];
        uint16_t rl = 0u;
        uint8_t server_nonce[16];
        uint8_t proof_in[36], proof[16], body2[32];

        CHECK(handshake_step1(&g_app_session, g_now_ms, server_nonce, rsp, &rl));
        memcpy(proof_in, server_nonce, 16u);
        memcpy(proof_in + 16, CLIENT_NONCE, 16u);
        memcpy(proof_in + 32, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN);
        host_cmac(MASTER, proof_in, 36u, proof);
        memcpy(body2, CLIENT_NONCE, 16u);
        memcpy(body2 + 16, proof, 16u);

        od_txq_reset();
        memset(g_sent, 0, sizeof g_sent);
        g_sent_n = 0u;
        CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
        o = od_gate_authenticate(&r, &BLE, od_span_make(body2, sizeof body2));
        CHECK(o == OD_FRAME_AUTH_ESTABLISHED);
        drain();
        CHECK(g_sent[0].len == OD_SESSION_STEP2_REPLY_LEN);
        CHECK(g_sent[0].data[2] == AUTH_STATUS_SUCCESS);
    }

    CASE("a wrong proof is AUTH_CONTROL, and the reply is still plaintext");
    setup(false);
    {
        uint8_t server_nonce[16];
        CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
        CHECK(handshake(&g_app_session, g_now_ms, server_nonce, true) == OD_SESSION_AUTH_REJECTED);
    }

    CASE("no security configured is AUTH_CONTROL with AUTH_STATUS_NOT_CONFIG");
    setup(false);
    g_security_on = false;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    o = od_gate_authenticate(&r, &BLE, od_span_make(step1, 1u));
    CHECK(o == OD_FRAME_AUTH_CONTROL);
    drain();
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[2] == AUTH_STATUS_NOT_CONFIG);
}

int main(void)
{
    test_open_ok();
    test_open_no_session();
    test_open_bad_tag();
    test_open_short_and_long();
    test_pipe_replay_is_silent();
    test_bad_scratch();
    test_authenticate_dispositions();

    printf("gate: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
