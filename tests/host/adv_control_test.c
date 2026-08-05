/* adv_control_test.c -- host tests for the loop-owned advertising controller.
 *
 * These were written BEFORE od_adv_control.c existed, against the header contract, per
 * docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md § "Implementation sequence" step 1. They encode the
 * host-acceptance list in that document; the ESP32 on-air acceptance is a separate,
 * hardware-only matter and nothing here should be read as covering it.
 *
 * WHAT A FAKE HAL CAN AND CANNOT PROVE. It proves the POLICY: ordering, intent survival,
 * payload coalescing, and error handling. It cannot prove that the target's AD packing is
 * byte-correct, that NimBLE accepts the call sequence, or that the event bridge publishes
 * coherently -- those need the Milestone 0 byte fixture and a board. Do not let a green run
 * here be quoted as F4 being closed.
 */
#include "od_adv_control.h"
#include "od_hal_adv.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- tiny harness --- */

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(cond)) {                                                                  \
            ++g_failures;                                                               \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);          \
        }                                                                               \
    } while (0)

#define CASE(name) (g_case = (name))

/* ----------------------------------------------------------------------------- fake HAL --- */

/* One scripted result per operation, plus a call log. The controller links against these at
 * link time exactly as a target would -- there is no injection seam, because shared/ binds
 * its HAL at link time by design (CLAUDE.md § "The one rule"). */
static enum od_hal_adv_result g_program_result;
static enum od_hal_adv_result g_start_result;
static enum od_hal_adv_result g_stop_result;

static unsigned g_program_calls;
static unsigned g_start_calls;
static unsigned g_stop_calls;
static uint8_t  g_programmed[OD_ADV_MSD_LEN];

static void fake_reset(void)
{
    g_program_result = OD_HAL_ADV_OK;
    g_start_result   = OD_HAL_ADV_OK;
    g_stop_result    = OD_HAL_ADV_OK;
    g_program_calls  = 0;
    g_start_calls    = 0;
    g_stop_calls     = 0;
    memset(g_programmed, 0, sizeof g_programmed);
}

static unsigned fake_total_calls(void)
{
    return g_program_calls + g_start_calls + g_stop_calls;
}

enum od_hal_adv_result od_hal_adv_program(const uint8_t msd[OD_ADV_MSD_LEN])
{
    ++g_program_calls;
    if (g_program_result == OD_HAL_ADV_OK) {
        memcpy(g_programmed, msd, OD_ADV_MSD_LEN);
    }
    return g_program_result;
}

enum od_hal_adv_result od_hal_adv_start(void)
{
    ++g_start_calls;
    return g_start_result;
}

enum od_hal_adv_result od_hal_adv_stop(void)
{
    ++g_stop_calls;
    return g_stop_result;
}

/* -------------------------------------------------------------------------------- helpers --- */

static void fill_msd(uint8_t out[OD_ADV_MSD_LEN], uint8_t seed)
{
    for (unsigned i = 0; i < OD_ADV_MSD_LEN; ++i) {
        out[i] = (uint8_t)(seed + i);
    }
}

/* Run process() until it stops acting, so a test can assert a settled state without hand-
 * counting passes. Bounded so a controller that never converges fails loudly instead of
 * hanging the suite -- an infinite reconcile loop is itself a defect worth catching. */
static unsigned settle(struct od_adv_control *s, bool start_allowed)
{
    unsigned passes = 0;
    while (passes < 32u) {
        enum od_adv_process_result r = od_adv_process(s, start_allowed);
        ++passes;
        if (r != OD_ADV_ACTED) {
            break;
        }
    }
    CHECK(passes < 32u);
    return passes;
}

/* Bring a controller to "advertising, payload applied" the ordinary way. */
static void bring_up(struct od_adv_control *s, uint8_t seed)
{
    uint8_t msd[OD_ADV_MSD_LEN];
    fill_msd(msd, seed);
    od_adv_control_init(s);
    od_adv_set_payload(s, msd);
    od_adv_request_start(s);
    od_adv_stack_ready(s);
    (void)settle(s, true);
}

/* ---------------------------------------------------------------------------------- tests --- */

/* Start requested before host sync must be REMEMBERED. This is the defect the ESP32
 * s_adv_wanted flag was introduced to fix: without stored intent, a start that landed before
 * sync did nothing and returned success, and the device stayed silent. */
static void test_start_before_ready(void)
{
    struct od_adv_control s;
    uint8_t msd[OD_ADV_MSD_LEN];

    CASE("start before ready is remembered");
    fake_reset();
    fill_msd(msd, 0x10);

    od_adv_control_init(&s);
    od_adv_set_payload(&s, msd);
    od_adv_request_start(&s);

    /* Nothing may touch the stack before it is ready. */
    (void)settle(&s, true);
    CHECK(fake_total_calls() == 0);
    CHECK(!s.active);

    od_adv_stack_ready(&s);
    (void)settle(&s, true);
    CHECK(g_program_calls == 1);
    CHECK(g_start_calls == 1);
    CHECK(s.active);
    CHECK(memcmp(g_programmed, msd, OD_ADV_MSD_LEN) == 0);
}

/* A stop must not be revivable by a late stack event. The deep-sleep teardown path depends on
 * this: an ADV_COMPLETE or a ready arriving after the application committed to stop must not
 * restart the radio. */
static void test_stop_dominates_late_events(void)
{
    struct od_adv_control s;

    CASE("stop dominates late ready/ended events");
    fake_reset();
    bring_up(&s, 0x20);
    CHECK(s.active);

    od_adv_request_stop(&s);
    (void)settle(&s, true);
    CHECK(g_stop_calls == 1);
    CHECK(!s.active);
    CHECK(od_adv_is_quiescent(&s));

    /* Every fact the stack can still deliver after the stop. */
    od_adv_observe_ended(&s);
    od_adv_stack_ready(&s);
    od_adv_set_connection_count(&s, 0);

    const unsigned starts_before = g_start_calls;
    (void)settle(&s, true);
    CHECK(g_start_calls == starts_before);
    CHECK(!s.active);
    CHECK(od_adv_is_quiescent(&s));
}

/* Several payload updates between passes must coalesce to the LATEST COMPLETE snapshot -- never
 * a mixture of two revisions, which is the torn-MSD half of F4. */
static void test_payload_coalesces_to_latest(void)
{
    struct od_adv_control s;
    uint8_t a[OD_ADV_MSD_LEN], b[OD_ADV_MSD_LEN], c[OD_ADV_MSD_LEN];

    CASE("payload updates coalesce to the latest complete snapshot");
    fake_reset();
    fill_msd(a, 0x30);
    fill_msd(b, 0x40);
    fill_msd(c, 0x50);

    od_adv_control_init(&s);
    od_adv_request_start(&s);
    od_adv_stack_ready(&s);
    od_adv_set_payload(&s, a);
    od_adv_set_payload(&s, b);
    od_adv_set_payload(&s, c);
    (void)settle(&s, true);

    CHECK(g_program_calls == 1);
    CHECK(memcmp(g_programmed, c, OD_ADV_MSD_LEN) == 0);

    /* An identical payload must not churn the stack. */
    const unsigned programs = g_program_calls;
    od_adv_set_payload(&s, c);
    (void)settle(&s, true);
    CHECK(g_program_calls == programs);
}

/* A payload change while advertising is stop / program / start -- the conservative sequence
 * every stack can express. */
static void test_payload_change_while_active(void)
{
    struct od_adv_control s;
    uint8_t next[OD_ADV_MSD_LEN];

    CASE("payload change while advertising restarts cleanly");
    fake_reset();
    bring_up(&s, 0x60);
    fill_msd(next, 0x70);

    const unsigned stops = g_stop_calls;
    od_adv_set_payload(&s, next);
    (void)settle(&s, true);

    CHECK(g_stop_calls == stops + 1);
    CHECK(g_program_calls == 2);
    CHECK(g_start_calls == 2);
    CHECK(s.active);
    CHECK(memcmp(g_programmed, next, OD_ADV_MSD_LEN) == 0);
}

/* A connection stops advertising; the restart must wait for start_allowed. This is the
 * disconnect-during-EPD-refresh case, expressed without the controller knowing what a refresh
 * is. */
static void test_restart_deferred_until_allowed(void)
{
    struct od_adv_control s;

    CASE("disconnect during refresh defers restart until allowed");
    fake_reset();
    bring_up(&s, 0x80);

    od_adv_set_connection_count(&s, 1);
    od_adv_observe_ended(&s);          /* the connection consumed the advertisement */
    (void)settle(&s, true);
    CHECK(!s.active);

    od_adv_set_connection_count(&s, 0);

    /* Refresh in progress: no new start. */
    const unsigned starts = g_start_calls;
    (void)settle(&s, false);
    CHECK(g_start_calls == starts);
    CHECK(!s.active);

    /* Refresh done. */
    (void)settle(&s, true);
    CHECK(g_start_calls == starts + 1);
    CHECK(s.active);
}

/* start_allowed gates a NEW start only. It must never stop a running advertisement. */
static void test_start_allowed_does_not_stop(void)
{
    struct od_adv_control s;

    CASE("start_allowed false does not stop a running advertisement");
    fake_reset();
    bring_up(&s, 0x90);

    const unsigned stops = g_stop_calls;
    (void)settle(&s, false);
    CHECK(g_stop_calls == stops);
    CHECK(s.active);
}

/* A stack reset invalidates applied state but must NOT erase application intent, so a later
 * ready resumes with the retained payload automatically. */
static void test_stack_reset_keeps_intent(void)
{
    struct od_adv_control s;
    uint8_t msd[OD_ADV_MSD_LEN];

    CASE("stack reset invalidates applied state, keeps intent");
    fake_reset();
    fill_msd(msd, 0xA0);
    bring_up(&s, 0xA0);

    od_adv_stack_reset(&s);
    CHECK(!s.active);
    CHECK(s.desired);          /* intent survives */
    CHECK(!s.stack_ready);

    const unsigned programs = g_program_calls;
    od_adv_stack_ready(&s);
    (void)settle(&s, true);
    CHECK(g_program_calls == programs + 1);   /* reprogrammed, not assumed */
    CHECK(s.active);
    CHECK(memcmp(g_programmed, msd, OD_ADV_MSD_LEN) == 0);
}

/* ALREADY and NOT_ACTIVE are idempotent SUCCESS. A controller that treated them as faults
 * would diverge from the stack after exactly the races this design exists to survive. */
static void test_idempotent_results_are_success(void)
{
    struct od_adv_control s;
    uint8_t msd[OD_ADV_MSD_LEN];

    CASE("ALREADY on start is success");
    fake_reset();
    fill_msd(msd, 0xB0);
    g_start_result = OD_HAL_ADV_ALREADY;

    od_adv_control_init(&s);
    od_adv_set_payload(&s, msd);
    od_adv_request_start(&s);
    od_adv_stack_ready(&s);
    (void)settle(&s, true);
    CHECK(s.active);
    CHECK(!s.faulted);

    CASE("NOT_ACTIVE on stop is success");
    g_stop_result = OD_HAL_ADV_NOT_ACTIVE;
    od_adv_request_stop(&s);
    (void)settle(&s, true);
    CHECK(!s.active);
    CHECK(!s.faulted);
    CHECK(od_adv_is_quiescent(&s));
}

/* RETRY is backpressure: it must leave state UNCHANGED and be re-attempted, never advance. */
static void test_retry_leaves_state_unchanged(void)
{
    struct od_adv_control s;
    uint8_t msd[OD_ADV_MSD_LEN];

    CASE("RETRY leaves state unchanged and retries");
    fake_reset();
    fill_msd(msd, 0xC0);
    g_program_result = OD_HAL_ADV_RETRY;

    od_adv_control_init(&s);
    od_adv_set_payload(&s, msd);
    od_adv_request_start(&s);
    od_adv_stack_ready(&s);

    CHECK(od_adv_process(&s, true) == OD_ADV_BUSY);
    CHECK(!s.active);
    CHECK(g_start_calls == 0);          /* must not skip ahead past a failed program */

    CHECK(od_adv_process(&s, true) == OD_ADV_BUSY);
    CHECK(g_program_calls == 2);        /* retried, not abandoned */

    g_program_result = OD_HAL_ADV_OK;
    (void)settle(&s, true);
    CHECK(s.active);
    CHECK(!s.faulted);
}

/* ERROR latches, so a hot loop cannot flood the log with the same failure every pass. */
static void test_error_latches(void)
{
    struct od_adv_control s;
    uint8_t msd[OD_ADV_MSD_LEN];

    CASE("hard error latches a fault");
    fake_reset();
    fill_msd(msd, 0xD0);
    g_start_result = OD_HAL_ADV_ERROR;

    od_adv_control_init(&s);
    od_adv_set_payload(&s, msd);
    od_adv_request_start(&s);
    od_adv_stack_ready(&s);

    (void)settle(&s, true);
    CHECK(s.faulted);
    CHECK(!s.active);

    const unsigned starts = g_start_calls;
    (void)settle(&s, true);
    CHECK(g_start_calls == starts);     /* latched: no re-attempt storm */

    CASE("stack reset clears the fault");
    od_adv_stack_reset(&s);
    CHECK(!s.faulted);
}

/* At most ONE stack-mutating call per pass, so every transition is observable and no pass can
 * block the loop with a burst of stack work. */
static void test_one_call_per_pass(void)
{
    struct od_adv_control s;
    uint8_t msd[OD_ADV_MSD_LEN];

    CASE("at most one HAL call per process() pass");
    fake_reset();
    fill_msd(msd, 0xE0);

    od_adv_control_init(&s);
    od_adv_set_payload(&s, msd);
    od_adv_request_start(&s);
    od_adv_stack_ready(&s);

    for (unsigned pass = 0; pass < 6u; ++pass) {
        const unsigned before = fake_total_calls();
        (void)od_adv_process(&s, true);
        CHECK(fake_total_calls() - before <= 1u);
    }
    CHECK(s.active);
}

/* Teardown: request stop, pump to quiescence. Facts arriving during the barrier may report,
 * but must not defeat the stop. */
static void test_teardown_barrier(void)
{
    struct od_adv_control s;

    CASE("teardown reaches quiescence despite in-flight events");
    fake_reset();
    bring_up(&s, 0xF0);
    CHECK(!od_adv_is_quiescent(&s));

    od_adv_request_stop(&s);
    od_adv_observe_ended(&s);           /* in flight, arrives mid-barrier */
    od_adv_set_connection_count(&s, 1);
    (void)settle(&s, true);

    CHECK(od_adv_is_quiescent(&s));
    CHECK(!s.active);
    CHECK(!s.desired);
}

/* Revisions are compared for EQUALITY only, so wraparound needs no ordering arithmetic. Drive
 * the counter past the wrap and confirm reconciliation still converges. */
static void test_revision_wrap(void)
{
    struct od_adv_control s;
    uint8_t msd[OD_ADV_MSD_LEN];

    CASE("revision wrap is safe (equality-only semantics)");
    fake_reset();
    bring_up(&s, 0x11);

    s.desired_revision = 0xFFFFFFFEu;
    s.applied_revision = 0xFFFFFFFEu;

    for (unsigned i = 0; i < 4u; ++i) {
        fill_msd(msd, (uint8_t)(0x11u + i + 1u));
        od_adv_set_payload(&s, msd);
        (void)settle(&s, true);
        CHECK(s.active);
        CHECK(memcmp(g_programmed, msd, OD_ADV_MSD_LEN) == 0);
    }
    CHECK(s.desired_revision == s.applied_revision);
}

/* A start with no payload ever supplied must not program garbage. */
static void test_no_start_without_payload(void)
{
    struct od_adv_control s;

    CASE("no advertisement without a payload");
    fake_reset();
    od_adv_control_init(&s);
    od_adv_request_start(&s);
    od_adv_stack_ready(&s);

    (void)settle(&s, true);
    CHECK(g_program_calls == 0);
    CHECK(g_start_calls == 0);
    CHECK(!s.active);
}

int main(void)
{
    test_start_before_ready();
    test_stop_dominates_late_events();
    test_payload_coalesces_to_latest();
    test_payload_change_while_active();
    test_restart_deferred_until_allowed();
    test_start_allowed_does_not_stop();
    test_stack_reset_keeps_intent();
    test_idempotent_results_are_success();
    test_retry_leaves_state_unchanged();
    test_error_latches();
    test_one_call_per_pass();
    test_teardown_barrier();
    test_revision_wrap();
    test_no_start_without_payload();

    printf("adv_control: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
