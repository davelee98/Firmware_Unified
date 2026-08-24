/* Shared buzzer runner: authority pitch/folding, parsing, scheduling and the 30 s cap. */

#include "od_buzzer.h"
#include "od_buzzer_app.h"
#include "opendisplay_structs.h"

#include "od_check.h"

#include <math.h>
#include <string.h>

#define LOG_MAX 512u

struct tone_event {
    uint8_t pin;
    uint32_t centihz;
    uint8_t duty;
};

struct enable_event {
    uint8_t pin;
    bool high;
};

static struct tone_event g_tones[LOG_MAX];
static uint8_t g_stop_pins[LOG_MAX];
static struct enable_event g_enables[LOG_MAX];
static unsigned g_tone_count;
static unsigned g_stop_count;
static unsigned g_enable_count;
static bool g_start_result;
static uint32_t g_now;

bool od_buzzer_app_tone_start(uint8_t pin, uint32_t centihz, uint8_t duty)
{
    if (g_tone_count < LOG_MAX) {
        g_tones[g_tone_count].pin = pin;
        g_tones[g_tone_count].centihz = centihz;
        g_tones[g_tone_count].duty = duty;
    }
    ++g_tone_count;
    return g_start_result;
}

void od_buzzer_app_tone_stop(uint8_t pin)
{
    if (g_stop_count < LOG_MAX) {
        g_stop_pins[g_stop_count] = pin;
    }
    ++g_stop_count;
}

void od_buzzer_app_enable_write(uint8_t pin, bool high)
{
    if (g_enable_count < LOG_MAX) {
        g_enables[g_enable_count].pin = pin;
        g_enables[g_enable_count].high = high;
    }
    ++g_enable_count;
}

static const struct od_buzzer_config ACTIVE_HIGH = {
    9u, 10u, OD_BUZZER_FLAG_ENABLE_ACTIVE_HIGH, 37u
};

static void setup(void)
{
    od_buzzer_stop();
    memset(g_tones, 0, sizeof g_tones);
    memset(g_stop_pins, 0, sizeof g_stop_pins);
    memset(g_enables, 0, sizeof g_enables);
    g_tone_count = 0u;
    g_stop_count = 0u;
    g_enable_count = 0u;
    g_start_result = true;
    g_now = 1000u;
}

static uint32_t service(void)
{
    return od_buzzer_service(g_now);
}

static uint32_t service_at_deadline(void)
{
    uint32_t delay = service();

    if (delay != OD_BUZZER_IDLE) {
        g_now += delay;
    }
    return delay;
}

static uint8_t reference_fold(uint8_t index)
{
    unsigned folded = index;

    if (index == 0u) {
        return 0u;
    }
    while (folded < 117u) {
        folded += 24u;
    }
    while (folded > 234u) {
        folded -= 24u;
    }
    return (uint8_t)folded;
}

static void test_frequency_authority(void)
{
    CASE("all indices use the exponential host scale after octave folding");
    for (unsigned index = 0u; index < 256u; index++) {
        const uint8_t folded = reference_fold((uint8_t)index);
        const uint32_t expected = folded == 0u ? 0u :
            (uint32_t)llround(100.0 * 13.75 * pow(2.0, (double)folded / 24.0));

        CHECK(od_buzzer_index_centihz((uint8_t)index) == expected);
    }
    CHECK(od_buzzer_index_centihz(120u) == 44000u);
    CHECK(od_buzzer_index_centihz(1u) == od_buzzer_index_centihz(121u));
    CHECK(od_buzzer_index_centihz(255u) == od_buzzer_index_centihz(231u));
}

static void test_validation(void)
{
    uint8_t p[257] = {0u};

    setup();
    CASE("malformed streams return the deployed parser errors without starting");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, 2u, g_now) == 1);
    p[2] = 0u;
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, 3u, g_now) == 4);
    p[2] = 1u;
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, 3u, g_now) == 5);
    p[3] = 1u;
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, 4u, g_now) == 5);
    p[3] = 0u;
    p[4] = 0xAAu;
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, 5u, g_now) == 6);

    memset(p, 0, sizeof p);
    p[2] = 1u;
    p[3] = 126u;                 /* 3-byte header + count + 252 step bytes = 256 */
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, 256u, g_now) == 0);
    od_buzzer_stop();

    memset(p, 0, sizeof p);
    p[2] = 2u;
    p[3] = 126u;                 /* 252 step bytes */
    p[256] = 0u;                 /* second empty pattern: structurally exact, 257 bytes */
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 5);
    CHECK(service() == OD_BUZZER_IDLE);
    CHECK(g_tone_count == 0u);
}

static void test_pattern_schedule(void)
{
    uint8_t p[] = {
        3u, 2u, 2u,
        2u, 120u, 2u, 0u, 1u,
        1u, 144u, 3u
    };
    static const uint32_t delays[] = {10u, 5u, 20u, 15u, 10u, 5u, 20u, 15u};

    setup();
    CASE("patterns, rests, gaps and outer repeats retain their distinct timing");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 0);
    for (unsigned i = 0u; i < sizeof delays / sizeof delays[0]; i++) {
        CHECK(service_at_deadline() == delays[i]);
    }
    CHECK(service() == OD_BUZZER_IDLE);
    CHECK(g_tone_count == 4u);
    CHECK(g_tones[0].centihz == 44000u);
    CHECK(g_tones[1].centihz == 88000u);
    CHECK(g_tones[2].centihz == 44000u);
    CHECK(g_tones[3].centihz == 88000u);
    CHECK(g_tones[0].pin == 9u && g_tones[0].duty == 37u);
    CHECK(g_enable_count == 7u);       /* six non-zero steps plus final disable */
    CHECK(g_enables[0].high);
    CHECK(!g_enables[6].high);
}

static void test_active_preemption(void)
{
    static const struct od_buzzer_config replacement = {
        11u, 12u, OD_BUZZER_FLAG_ENABLE_ACTIVE_HIGH, 42u
    };
    uint8_t original[] = {0u, 1u, 1u, 1u, 120u, 10u};
    uint8_t malformed[] = {0u, 1u, 0u};
    uint8_t next[] = {0u, 1u, 1u, 1u, 144u, 2u};
    unsigned stop_count;
    unsigned enable_count;

    setup();
    CASE("malformed activation preserves an active run; valid activation replaces it");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, original, sizeof original, g_now) == 0);
    CHECK(service() == 50u);
    CHECK(g_tone_count == 1u);
    stop_count = g_stop_count;
    enable_count = g_enable_count;

    g_now += 20u;
    CHECK(od_buzzer_activate(&replacement, malformed, sizeof malformed, g_now) == 4);
    CHECK(g_stop_count == stop_count);
    CHECK(g_enable_count == enable_count);
    CHECK(service() == 30u);
    CHECK(g_tone_count == 1u);

    CHECK(od_buzzer_activate(&replacement, next, sizeof next, g_now) == 0);
    CHECK(g_stop_count == stop_count + 1u);
    CHECK(g_stop_pins[stop_count] == ACTIVE_HIGH.drive_pin);
    CHECK(g_enables[g_enable_count - 1u].pin == ACTIVE_HIGH.enable_pin);
    CHECK(!g_enables[g_enable_count - 1u].high);
    CHECK(service() == 10u);
    CHECK(g_tone_count == 2u);
    CHECK(g_tones[1].pin == replacement.drive_pin);
    CHECK(g_tones[1].centihz == 88000u);
    CHECK(g_tones[1].duty == replacement.duty_percent);
}

static void test_early_call_and_wrap(void)
{
    uint8_t p[] = {0u, 1u, 1u, 1u, 120u, 2u};

    setup();
    g_now = UINT32_MAX - 5u;
    CASE("relative deadlines are early-call safe across uint32 wrap");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 0);
    CHECK(service() == 10u);
    CHECK(g_tone_count == 1u);
    g_now += 5u;
    CHECK(service() == 5u);
    CHECK(g_tone_count == 1u);
    g_now += 5u;
    CHECK(service() == OD_BUZZER_IDLE);
    CHECK(g_tone_count == 1u);
}

static void test_tone_stop_transitions(void)
{
    uint8_t tone_gap[] = {0u, 1u, 2u, 1u, 120u, 2u, 1u, 144u, 2u};
    uint8_t tone_rest[] = {0u, 1u, 1u, 2u, 120u, 2u, 0u, 2u};

    setup();
    CASE("a tone is stopped before an inter-pattern gap");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, tone_gap, sizeof tone_gap, g_now) == 0);
    CHECK(service() == 10u);
    CHECK(g_stop_count == 0u);
    g_now += 10u;
    CHECK(service() == OD_BUZZER_INTER_PATTERN_MS);
    CHECK(g_stop_count == 1u);
    CHECK(g_stop_pins[0] == ACTIVE_HIGH.drive_pin);

    setup();
    CASE("a tone is stopped at expiry and remains stopped through a rest");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, tone_rest, sizeof tone_rest, g_now) == 0);
    CHECK(service() == 10u);
    CHECK(g_stop_count == 0u);
    g_now += 10u;
    CHECK(service() == 10u);
    CHECK(g_stop_count == 2u);
    CHECK(g_stop_pins[0] == ACTIVE_HIGH.drive_pin);
    CHECK(g_stop_pins[1] == ACTIVE_HIGH.drive_pin);
}

static void test_late_call_slips(void)
{
    uint8_t p[] = {0u, 1u, 1u, 2u, 120u, 2u, 144u, 2u};

    setup();
    CASE("a late service call starts one next step without catching up");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 0);
    CHECK(service() == 10u);
    g_now += 15u;
    CHECK(service() == 10u);
    CHECK(g_tone_count == 2u);
    g_now += 5u;
    CHECK(service() == 5u);
    CHECK(g_tone_count == 2u);
}

static void test_zero_duration_and_copy(void)
{
    uint8_t p[] = {0u, 1u, 1u, 3u, 117u, 0u, 144u, 0u, 120u, 1u};

    setup();
    CASE("zero-duration steps are no-ops and the accepted payload is owned");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 0);
    memset(p, 0, sizeof p);
    CHECK(service() == 5u);
    CHECK(g_tone_count == 1u);
    CHECK(g_tones[0].centihz == 44000u);
    g_now += 5u;
    CHECK(service() == OD_BUZZER_IDLE);
}

static void test_enable_polarity_and_start_failure(void)
{
    const struct od_buzzer_config active_low = {9u, 10u, 0u, 0u};
    uint8_t p[] = {0u, 0u, 1u, 1u, 120u, 1u};

    setup();
    g_start_result = false;
    CASE("active-low enable and a rejected tone still keep melody time");
    CHECK(od_buzzer_activate(&active_low, p, sizeof p, g_now) == 0);
    CHECK(service() == 5u);
    CHECK(g_tone_count == 1u);
    CHECK(g_enables[0].high == false);
    g_now += 5u;
    CHECK(service() == OD_BUZZER_IDLE);
    CHECK(g_enables[g_enable_count - 1u].high == true);
}

static void test_total_cap(void)
{
    uint8_t p[] = {0u, 255u, 1u, 1u, 120u, 255u};
    uint32_t elapsed = 0u;
    uint32_t last_delay = 0u;

    setup();
    g_now = 0u;
    CASE("authority total cap is exactly 30000 ms, including a clamped last step");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 0);
    for (;;) {
        const uint32_t delay = service();

        if (delay == OD_BUZZER_IDLE) {
            break;
        }
        last_delay = delay;
        elapsed += delay;
        g_now += delay;
    }
    CHECK(OD_BUZZER_MAX_TOTAL_MS == 30000u);
    CHECK(elapsed == 30000u);
    CHECK(last_delay == 675u);
    CHECK(g_tone_count == 24u);
    CHECK(g_now == 30000u);
}

static void test_total_cap_across_wrap(void)
{
    uint8_t p[] = {0u, 255u, 1u, 1u, 120u, 255u};
    const uint32_t started_ms = UINT32_MAX - 10000u;
    uint32_t elapsed = 0u;

    setup();
    g_now = started_ms;
    CASE("the 30000 ms cap uses elapsed time across uint32 wrap");
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 0);
    for (;;) {
        const uint32_t delay = service();

        if (delay == OD_BUZZER_IDLE) {
            break;
        }
        elapsed += delay;
        g_now += delay;
    }
    CHECK(elapsed == OD_BUZZER_MAX_TOTAL_MS);
    CHECK(g_now == started_ms + OD_BUZZER_MAX_TOTAL_MS);
    CHECK(g_tone_count == 24u);
}

static void test_total_cap_during_gap(void)
{
    uint8_t p[55] = {0u, 1u, 2u, 24u};
    unsigned offset = 4u;

    for (unsigned step = 0u; step < 23u; step++) {
        p[offset++] = 120u;
        p[offset++] = 255u;
    }
    p[offset++] = 120u;
    p[offset++] = 133u;
    p[offset++] = 1u;
    p[offset++] = 144u;
    p[offset++] = 1u;

    setup();
    g_now = 0u;
    CASE("the fixed inter-pattern gap may cross the cap but no next tone starts");
    CHECK(offset == sizeof p);
    CHECK(od_buzzer_activate(&ACTIVE_HIGH, p, sizeof p, g_now) == 0);
    while (g_now < 29990u) {
        const uint32_t delay = service();

        CHECK(delay != OD_BUZZER_IDLE);
        g_now += delay;
    }
    CHECK(g_now == 29990u);
    CHECK(g_tone_count == 24u);
    CHECK(service() == OD_BUZZER_INTER_PATTERN_MS);
    CHECK(g_tone_count == 24u);
    g_now += OD_BUZZER_INTER_PATTERN_MS;
    CHECK(service() == OD_BUZZER_IDLE);
    CHECK(g_now == 30010u);
    CHECK(g_tone_count == 24u);
    CHECK(!g_enables[g_enable_count - 1u].high);
}

int main(void)
{
    test_frequency_authority();
    test_validation();
    test_pattern_schedule();
    test_active_preemption();
    test_early_call_and_wrap();
    test_tone_stop_transitions();
    test_late_call_slips();
    test_zero_duration_and_copy();
    test_enable_polarity_and_start_failure();
    test_total_cap();
    test_total_cap_across_wrap();
    test_total_cap_during_gap();
    return OD_CHECK_REPORT_NONEMPTY("buzzer_test", 340u);
}
