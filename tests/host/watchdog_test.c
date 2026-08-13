/* watchdog_test.c -- host tests for the portable watchdog policy.
 *
 * WHAT A FAKE HAL CAN AND CANNOT PROVE. It proves the POLICY: the retained-byte layout, strike
 * accumulation and clearing, safe-mode entry and exit, breadcrumb caching, and degradation when
 * retained storage is unavailable. It cannot prove that a target's WDT registers are programmed
 * correctly, that a real GPREGRET2 survives a real reset, or that the timeout exceeds a real
 * panel refresh. Those need silicon. Do not let a green run here be quoted as "the watchdog
 * works" -- no target implements od_hal_wdt.h yet.
 *
 * The boot-loop cases are the ones that matter most. A watchdog that cannot escape a boot loop
 * is worse than no watchdog, so the tests below drive three consecutive watchdog resets and
 * assert the escape hatch opens, then assert it closes again on healthy uptime.
 */
#include "od_watchdog.h"
#include "od_hal_wdt.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- tiny harness --- */

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

/* ----------------------------------------------------------------------------- fake HAL --- */

/* Supplied at LINK time, exactly as a target does -- shared/ binds its HAL at link time by
 * design, so there is no injection seam to fake around and the test exercises real linkage. */
static uint32_t g_reset_reason;
static uint8_t  g_retained;
static bool     g_retained_readable;
static bool     g_retained_writable;
static unsigned g_feed_calls;
static unsigned g_arm_calls;
static uint32_t g_arm_timeout;
static enum od_hal_wdt_arm_result g_arm_result;

static void fake_reset(void)
{
    g_reset_reason      = OD_HAL_WDT_RESET_POWER_ON;
    g_retained          = 0u;
    g_retained_readable = true;
    g_retained_writable = true;
    g_feed_calls        = 0u;
    g_arm_calls         = 0u;
    g_arm_timeout       = 0u;
    g_arm_result        = OD_HAL_WDT_ARM_OK;
}

uint32_t od_hal_wdt_reset_reason(void)
{
    return g_reset_reason;
}

bool od_hal_wdt_retained_read(uint8_t *out)
{
    if (!g_retained_readable) {
        return false;
    }
    *out = g_retained;
    return true;
}

bool od_hal_wdt_retained_write(uint8_t value)
{
    if (!g_retained_writable) {
        return false;
    }
    g_retained = value;
    return true;
}

enum od_hal_wdt_arm_result od_hal_wdt_arm(uint32_t timeout_s)
{
    ++g_arm_calls;
    g_arm_timeout = timeout_s;
    return g_arm_result;
}

void od_hal_wdt_feed(void)
{
    ++g_feed_calls;
}

/* ------------------------------------------------------------------- retained byte layout --- */

static void test_retained_layout(void)
{
    uint8_t b;

    CASE("layout: pack round-trips strikes and phase");
    b = od_watchdog_retained_pack(2u, (uint8_t)OD_WDT_PHASE_STREAM);
    CHECK(od_watchdog_retained_valid(b));
    CHECK(od_watchdog_retained_strikes(b) == 2u);
    CHECK(od_watchdog_retained_phase(b) == (uint8_t)OD_WDT_PHASE_STREAM);

    CASE("layout: the tag is 0b10 in the top two bits");
    CHECK((b & OD_WDT_RETAINED_TAG_MASK) == 0x80u);

    CASE("layout: an untagged byte is invalid, whatever it contains");
    CHECK(!od_watchdog_retained_valid(0x00u));
    CHECK(!od_watchdog_retained_valid(0xFFu)); /* tag 0b11 */
    CHECK(!od_watchdog_retained_valid(0x3Fu)); /* tag 0b00 */

    CASE("layout: strikes saturate at 3 rather than truncating to 0");
    /* 4 would truncate to 0 in two bits, silently clearing the counter at the moment it should
     * trip safe mode. Saturation is the whole point of this assertion. */
    b = od_watchdog_retained_pack(4u, (uint8_t)OD_WDT_PHASE_IDLE);
    CHECK(od_watchdog_retained_strikes(b) == 3u);

    CASE("layout: phase and strikes do not overlap");
    b = od_watchdog_retained_pack(3u, (uint8_t)OD_WDT_PHASE_PWRMGM_BUS);
    CHECK(od_watchdog_retained_strikes(b) == 3u);
    CHECK(od_watchdog_retained_phase(b) == 15u);
}

static void test_phase_names(void)
{
    CASE("phase names: every defined phase has a name");
    for (uint8_t p = 0; p <= (uint8_t)OD_WDT_PHASE__MAX; p++) {
        const char *n = od_watchdog_phase_name(p);
        CHECK(n != NULL);
        CHECK(strcmp(n, "UNKNOWN") != 0);
    }

    CASE("phase names: out of range is UNKNOWN, not a read off the end");
    CHECK(strcmp(od_watchdog_phase_name(16u), "UNKNOWN") == 0);
    CHECK(strcmp(od_watchdog_phase_name(255u), "UNKNOWN") == 0);
}

/* -------------------------------------------------------------------------------- boot --- */

static void test_cold_boot(void)
{
    struct od_watchdog s;
    struct od_watchdog_boot_report r;

    CASE("cold boot: no breadcrumb, no strikes, no safe mode");
    fake_reset();
    od_watchdog_boot_init(&s, 0u, &r);
    CHECK(!r.breadcrumb_valid);
    CHECK(r.strikes == 0u);
    CHECK(!r.safe_mode);
    CHECK(!od_watchdog_in_safe_mode(&s));
    CHECK(r.retained_readable);
    CHECK(r.retained_writable);

    CASE("cold boot: the tag is established so later breadcrumbs have a byte to modify");
    CHECK(od_watchdog_retained_valid(g_retained));

    CASE("cold boot: IDLE is stamped");
    CHECK(od_watchdog_retained_phase(g_retained) == (uint8_t)OD_WDT_PHASE_IDLE);
}

static void test_breadcrumb_survives(void)
{
    struct od_watchdog s;
    struct od_watchdog_boot_report r;

    CASE("boot: a surviving breadcrumb is reported as the previous phase");
    fake_reset();
    g_retained = od_watchdog_retained_pack(0u, (uint8_t)OD_WDT_PHASE_REFRESH_WAIT);
    g_reset_reason = OD_HAL_WDT_RESET_WATCHDOG;
    od_watchdog_boot_init(&s, 0u, &r);
    CHECK(r.breadcrumb_valid);
    CHECK(r.prev_phase == (uint8_t)OD_WDT_PHASE_REFRESH_WAIT);
    CHECK(r.was_watchdog);
}

/* ------------------------------------------------------------------ strikes and safe mode --- */

/* Simulate a reset: the retained byte persists, only the struct is rebuilt. */
static void reboot(struct od_watchdog *s, struct od_watchdog_boot_report *r,
                   uint32_t reason, uint32_t now_ms)
{
    g_reset_reason = reason;
    od_watchdog_boot_init(s, now_ms, r);
}

static void test_strikes_accumulate_to_safe_mode(void)
{
    struct od_watchdog s;
    struct od_watchdog_boot_report r;

    CASE("strikes: three consecutive watchdog resets open the escape hatch");
    fake_reset();

    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(r.strikes == 1u);
    CHECK(!r.safe_mode);

    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(r.strikes == 2u);
    CHECK(!r.safe_mode);

    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(r.strikes == 3u);
    CHECK(r.safe_mode);
    CHECK(od_watchdog_in_safe_mode(&s));

    CASE("strikes: the counter saturates at 3 and stays in safe mode");
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(r.strikes == 3u);
    CHECK(r.safe_mode);
}

static void test_non_watchdog_reset_clears(void)
{
    struct od_watchdog s;
    struct od_watchdog_boot_report r;

    CASE("strikes: any non-watchdog reset breaks the cycle and starts clean");
    fake_reset();
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(r.strikes == 2u);

    reboot(&s, &r, OD_HAL_WDT_RESET_PIN, 0u);
    CHECK(r.strikes == 0u);
    CHECK(!r.safe_mode);

    CASE("strikes: a power-on also clears");
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(r.strikes == 1u);
    reboot(&s, &r, OD_HAL_WDT_RESET_POWER_ON, 0u);
    CHECK(r.strikes == 0u);
}

static void test_healthy_uptime_clears_strikes(void)
{
    struct od_watchdog s;
    struct od_watchdog_boot_report r;

    CASE("uptime: strikes survive a short run");
    fake_reset();
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 1000u);
    CHECK(r.strikes == 1u);
    od_watchdog_feed(&s, 1000u + (uint32_t)OD_WDT_HEALTHY_MS - 1u);
    CHECK(od_watchdog_retained_strikes(g_retained) == 1u);

    CASE("uptime: reaching the healthy threshold clears them");
    od_watchdog_feed(&s, 1000u + (uint32_t)OD_WDT_HEALTHY_MS);
    CHECK(od_watchdog_retained_strikes(g_retained) == 0u);

    CASE("uptime: safe mode is self-exiting across the next boot");
    /* Safe mode itself does no panel work, so an uptime rule is the only thing that can clear
     * it -- a refresh-based rule could never be satisfied from inside safe mode. */
    fake_reset();
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(od_watchdog_in_safe_mode(&s));
    od_watchdog_feed(&s, (uint32_t)OD_WDT_HEALTHY_MS);
    CHECK(od_watchdog_retained_strikes(g_retained) == 0u);
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(r.strikes == 1u);
    CHECK(!od_watchdog_in_safe_mode(&s));

    CASE("uptime: a wrapping millisecond counter is a non-event");
    fake_reset();
    reboot(&s, &r, OD_HAL_WDT_RESET_WATCHDOG, 0xFFFFF000u);
    CHECK(r.strikes == 1u);
    od_watchdog_feed(&s, 0xFFFFF000u + (uint32_t)OD_WDT_HEALTHY_MS); /* wraps */
    CHECK(od_watchdog_retained_strikes(g_retained) == 0u);
}

/* -------------------------------------------------------------------------- breadcrumbs --- */

static void test_breadcrumb_caching(void)
{
    struct od_watchdog s;

    CASE("breadcrumb: an unchanged phase does not rewrite");
    fake_reset();
    od_watchdog_boot_init(&s, 0u, NULL);
    od_watchdog_breadcrumb(&s, (uint8_t)OD_WDT_PHASE_FILL);
    CHECK(od_watchdog_retained_phase(g_retained) == (uint8_t)OD_WDT_PHASE_FILL);

    /* Make writes fail, then restamp the SAME phase. A cached no-op must not touch the store,
     * so the failure latch must stay clear. */
    g_retained_writable = false;
    od_watchdog_breadcrumb(&s, (uint8_t)OD_WDT_PHASE_FILL);
    CHECK(!s.write_failed);

    CASE("breadcrumb: a failed write does not advance the cache, so the next attempt retries");
    od_watchdog_breadcrumb(&s, (uint8_t)OD_WDT_PHASE_STREAM);
    CHECK(s.write_failed);
    CHECK(od_watchdog_retained_phase(g_retained) == (uint8_t)OD_WDT_PHASE_FILL);

    g_retained_writable = true;
    od_watchdog_breadcrumb(&s, (uint8_t)OD_WDT_PHASE_STREAM);
    CHECK(od_watchdog_retained_phase(g_retained) == (uint8_t)OD_WDT_PHASE_STREAM);

    CASE("breadcrumb: stamping never disturbs the strike count");
    fake_reset();
    reboot(&s, NULL, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    reboot(&s, NULL, OD_HAL_WDT_RESET_WATCHDOG, 0u);
    CHECK(od_watchdog_retained_strikes(g_retained) == 2u);
    od_watchdog_breadcrumb(&s, (uint8_t)OD_WDT_PHASE_REFRESH_WAIT);
    CHECK(od_watchdog_retained_strikes(g_retained) == 2u);
    CHECK(od_watchdog_retained_phase(g_retained) == (uint8_t)OD_WDT_PHASE_REFRESH_WAIT);
}

/* ------------------------------------------------------------------------ degraded store --- */

static void test_retained_unavailable(void)
{
    struct od_watchdog s;
    struct od_watchdog_boot_report r;

    CASE("no retained store: boot reports it rather than pretending");
    fake_reset();
    g_retained_readable = false;
    g_retained_writable = false;
    od_watchdog_boot_init(&s, 0u, &r);
    CHECK(!r.retained_readable);
    CHECK(!r.breadcrumb_valid);
    CHECK(r.strikes == 0u);
    CHECK(!r.safe_mode);

    CASE("no retained store: feeding and stamping stay safe");
    od_watchdog_feed(&s, 1000u);
    od_watchdog_breadcrumb(&s, (uint8_t)OD_WDT_PHASE_STREAM);
    CHECK(g_feed_calls == 1u);
}

/* -------------------------------------------------------------------------------- arming --- */

static void test_arm(void)
{
    struct od_watchdog s;

    CASE("arm: the configured timeout reaches the HAL");
    fake_reset();
    od_watchdog_boot_init(&s, 0u, NULL);
    CHECK(od_watchdog_arm(&s) == OD_HAL_WDT_ARM_OK);
    CHECK(g_arm_calls == 1u);
    CHECK(g_arm_timeout == (uint32_t)OD_WDT_TIMEOUT_S);
    CHECK(s.armed);

    CASE("arm: an INHERITED watchdog counts as armed");
    /* Something is running and will reset this device whether or not this build started it.
     * Treating inherit as "not armed" is the brick path described in od_hal_wdt.h. */
    fake_reset();
    g_arm_result = OD_HAL_WDT_ARM_INHERITED;
    od_watchdog_boot_init(&s, 0u, NULL);
    CHECK(od_watchdog_arm(&s) == OD_HAL_WDT_ARM_INHERITED);
    CHECK(s.armed);

    CASE("arm: disabled and unsupported are not armed");
    fake_reset();
    g_arm_result = OD_HAL_WDT_ARM_DISABLED;
    od_watchdog_boot_init(&s, 0u, NULL);
    (void)od_watchdog_arm(&s);
    CHECK(!s.armed);

    fake_reset();
    g_arm_result = OD_HAL_WDT_ARM_UNSUPPORTED;
    od_watchdog_boot_init(&s, 0u, NULL);
    (void)od_watchdog_arm(&s);
    CHECK(!s.armed);

    CASE("feed: reaches the HAL even when nothing of ours is armed");
    /* The HAL is the only layer that can see an inherited watchdog this module never armed, so
     * a gate here would suppress exactly the feed that prevents the brick. */
    fake_reset();
    g_arm_result = OD_HAL_WDT_ARM_DISABLED;
    od_watchdog_boot_init(&s, 0u, NULL);
    (void)od_watchdog_arm(&s);
    od_watchdog_feed(&s, 0u);
    CHECK(g_feed_calls == 1u);
}

/* -------------------------------------------------------------------------------- null --- */

static void test_null_safety(void)
{
    CASE("null: every entry point tolerates a NULL state");
    fake_reset();
    od_watchdog_boot_init(NULL, 0u, NULL);
    od_watchdog_feed(NULL, 0u);
    od_watchdog_breadcrumb(NULL, 0u);
    CHECK(!od_watchdog_in_safe_mode(NULL));
    CHECK(od_watchdog_arm(NULL) == OD_HAL_WDT_ARM_ERROR);
}

int main(void)
{
    test_retained_layout();
    test_phase_names();
    test_cold_boot();
    test_breadcrumb_survives();
    test_strikes_accumulate_to_safe_mode();
    test_non_watchdog_reset_clears();
    test_healthy_uptime_clears_strikes();
    test_breadcrumb_caching();
    test_retained_unavailable();
    test_arm();
    test_null_safety();

    printf("%s: %u checks, %u failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
