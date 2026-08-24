/* od_led — the shared LED runner's yield contract and group-repeat sentinel.
 *
 * `od_led_service()` advances a phase machine. A phase that neither emits nor waits would fall
 * through and re-enter the loop, so a pattern whose delays are all zero could drive it forever --
 * on BG22 that is a superloop with no watchdog, and the pattern arrives over the wire.
 *
 * The bound asserted here is per SERVICE CALL, counted in yield-requiring actions -- LED emissions
 * and scheduled waits -- not in phase transitions. Zero-work transitions (GROUP -> LOOP1 -> LOOP2
 * with empty loops) are legitimate inside one call; an unbounded number of them is not.
 *
 * Written against the BG22 runner before promotion; repointed at the shared machine unchanged,
 * which is what the promotion had to preserve.
 */

#include "od_led.h"
#include "od_led_app.h"

#include "od_check.h"

#include <string.h>

/* ------------------------------------------------------------------------- the seam --- */

static unsigned g_pin_writes;       /* every level change the runner performs */
static unsigned g_pwm_waits;        /* led_flash issues exactly LED_PWM_WAITS_PER_FLASH */
static uint8_t g_mode;              /* the live nibble od_led_app_mode() reports */
static unsigned g_finished_calls;
static uint8_t g_finished_instance;

/* One flash is one PWM pass per brightness step, and each pass issues a fixed number of
 * micro-waits -- so counting these counts emissions without depending on colour or pin count. */
#define LED_PWM_WAITS_PER_FLASH 7u

#define WRITE_LOG_MAX 512
static struct { uint8_t pin; bool level; } g_writes[WRITE_LOG_MAX];
static unsigned g_write_n;

void od_led_app_write(uint8_t pin_cfg, bool level_high)
{
    if (pin_cfg == 0xFFu) {
        return;                          /* absent pins are never driven */
    }
    ++g_pin_writes;
    if (g_write_n < WRITE_LOG_MAX) {
        g_writes[g_write_n].pin = pin_cfg;
        g_writes[g_write_n].level = level_high;
        ++g_write_n;
    }
}

uint8_t od_led_app_mode(uint8_t instance) { (void)instance; return g_mode; }

void od_led_app_finished(uint8_t instance)
{
    ++g_finished_calls;
    g_finished_instance = instance;
    g_mode = 0u;                    /* the target clears the nibble */
}

void od_hal_delay_us(uint32_t us) { (void)us; ++g_pwm_waits; }

/* ---------------------------------------------------------------------- the fixture --- */

static uint8_t g_pattern[OD_LED_PATTERN_LEN];
static uint32_t g_now;

static const struct od_led_pins g_pins = {
    0x00u,          /* red   -- port A pin 0 */
    0xFFu,          /* green -- absent */
    0xFFu,          /* blue  -- absent */
    0x00u           /* no inversion */
};

static void set_pattern(uint8_t group_repeats_raw, uint8_t loopcnt, uint8_t loopdelay,
                        uint8_t ildelay)
{
    memset(g_pattern, 0, sizeof g_pattern);
    g_pattern[0] = 0x01u;                            /* mode 1 = run, brightness nibble 0 -> 1 */
    g_pattern[1] = g_pattern[4] = g_pattern[7] = 0x01u;
    g_pattern[2] = g_pattern[5] = g_pattern[8] = (uint8_t)((loopdelay << 4) | (loopcnt & 0x0Fu));
    g_pattern[3] = g_pattern[6] = g_pattern[9] = ildelay;
    g_pattern[10] = group_repeats_raw;
}

static void setup(void)
{
    /* Reset through the production stop path; do not keep a production reset API alive solely
     * for this fixture. Any writes/completion from stopping the previous case are discarded by
     * the counter reset below. */
    (void)od_led_stop(0u, false);
    g_pin_writes = 0u;
    g_write_n = 0u;
    g_pwm_waits = 0u;
    g_finished_calls = 0u;
    g_finished_instance = 0xFFu;
    g_mode = 0x01u;
    g_now = 1000u;
}

static bool pattern_running(void) { return g_mode == 0x01u; }

/* Advance to the deadline the machine asked for, then service. Returns the waits it scheduled. */
static unsigned service_once(void)
{
    const uint32_t delay = od_led_service(g_now);

    if (delay == OD_LED_IDLE) {
        return 0u;
    }
    g_now += delay;
    return 1u;
}

/* -------------------------------------------------------------------------- the cases --- */

/* The degenerate pattern: every delay zero and an endless group, so no phase configures a wait.
 * A test cannot detect an unbounded loop directly -- it would hang with it -- so it asserts the
 * bound that makes one impossible instead. */
static void test_all_zero_delays_yield(void)
{
    static const uint8_t endless[] = { 0xFEu, 0xFFu };

    for (unsigned i = 0; i < sizeof endless / sizeof endless[0]; ++i) {
        CASE("an all-zero-delay endless pattern yields once per service call");
        setup();
        set_pattern(endless[i], 0u, 0u, 0u);         /* no flashes, no waits configured */
        CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
        CHECK(pattern_running());

        for (unsigned n = 0; n < 32u; ++n) {
            CHECK(od_led_service(g_now) == OD_LED_MIN_STEP_DELAY_MS);
            g_now += OD_LED_MIN_STEP_DELAY_MS;
        }
        /* Still running: an endless pattern must not be terminated by the floor. */
        CHECK(pattern_running());
        CHECK(g_finished_calls == 0u);
    }
}

/* A flashing pattern with no configured inter-flash delay is the other non-yielding path. */
static void test_zero_delay_flashes_yield(void)
{
    const unsigned brightness = 1u;                  /* set_pattern leaves the high nibble 0 */

    CASE("a zero-delay flashing pattern emits at most one flash per service call");
    setup();
    set_pattern(0xFEu, 3u, 0u, 0u);                  /* three flashes per loop, no delays */
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);

    for (unsigned n = 0; n < 24u; ++n) {
        const unsigned waits_before = g_pwm_waits;

        CHECK(od_led_service(g_now) == OD_LED_MIN_STEP_DELAY_MS);
        g_now += OD_LED_MIN_STEP_DELAY_MS;
        /* At most one flash: the group-closing call emits none, every other call emits one. */
        CHECK(g_pwm_waits - waits_before <= LED_PWM_WAITS_PER_FLASH * brightness);
    }
}

/* The sentinel. Raw 0xFE and 0xFF both mean indefinite, matching py-opendisplay's encoder and
 * decoder. A finite count runs exactly count+1 groups -- asserted as an emission total, because
 * "it stopped eventually" is equally true of a runner that stops after one group. */
static void test_group_repeat_sentinels(void)
{
    static const struct { uint8_t raw; bool endless; unsigned groups; } cases[] = {
        { 0xFEu, true,  0u }, { 0xFFu, true,  0u },
        { 0x00u, false, 1u }, { 0x02u, false, 3u }, { 0x05u, false, 6u },
    };
    const unsigned brightness = 1u;
    const unsigned flashes_per_group = 3u;           /* one per loop, loopcnt 1 each */

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        CASE("group_repeats: 0xFE and 0xFF are endless, a finite count runs count+1 groups");
        setup();
        set_pattern(cases[i].raw, 1u, 0u, 0u);
        CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);

        for (unsigned n = 0; n < 256u && pattern_running(); ++n) {
            (void)service_once();
        }
        CHECK(pattern_running() == cases[i].endless);

        if (cases[i].endless) {
            continue;
        }
        CHECK(g_pwm_waits ==
              cases[i].groups * flashes_per_group * LED_PWM_WAITS_PER_FLASH * brightness);
        /* Completion is reported once, for the running instance, so the target clears its byte. */
        CHECK(g_finished_calls == 1u);
        CHECK(g_finished_instance == 0u);
    }
}

/* A configured delay is honoured, and an early call must not run ahead of it. */
static void test_configured_delay_is_honoured(void)
{
    CASE("a configured delay holds the runner until it elapses");
    setup();
    set_pattern(0xFEu, 1u, 2u, 0u);                  /* loop delay 2 * 100 ms */
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
    CHECK(od_led_service(g_now) == 200u);            /* one flash, then the configured wait */

    CASE("an early call returns the remaining delay and advances nothing");
    {
        const unsigned waits_before = g_pwm_waits;

        CHECK(od_led_service(g_now + 50u) == 150u);
        CHECK(g_pwm_waits == waits_before);
        CHECK(od_led_service(g_now + 199u) == 1u);
        CHECK(g_pwm_waits == waits_before);
    }

    CASE("the call at the deadline advances");
    CHECK(od_led_service(g_now + 200u) != OD_LED_IDLE);
}

/* The mode nibble is read live: clearing it out of band stops the pattern. */
static void test_external_clear_stops(void)
{
    CASE("clearing the mode nibble stops a running pattern");
    setup();
    set_pattern(0xFEu, 1u, 0u, 0u);
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
    CHECK(service_once() == 1u);

    g_mode = 0u;                                     /* a config write clears it */
    CHECK(od_led_service(g_now) == OD_LED_IDLE);
    /* Not a completion: nothing to clear, and re-clearing would race whoever cleared it. */
    CHECK(g_finished_calls == 0u);

    CASE("a stopped pattern stays stopped");
    CHECK(od_led_service(g_now) == OD_LED_IDLE);
}

/* Wrap-safety: the deadline comparison must survive the uint32 millisecond rollover. */
static void test_millisecond_wrap(void)
{
    CASE("a deadline that wraps past 2^32 is still honoured");
    setup();
    g_now = 0xFFFFFF00u;                             /* 256 ms before the wrap */
    set_pattern(0xFEu, 1u, 2u, 0u);                  /* 200 ms wait, so the deadline wraps */
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
    CHECK(od_led_service(g_now) == 200u);

    {
        const unsigned waits_before = g_pwm_waits;

        /* Still early, on the far side of the wrap. */
        CHECK(od_led_service(g_now + 100u) == 100u);
        CHECK(g_pwm_waits == waits_before);
        /* At the deadline, which has wrapped to a small number. */
        CHECK(od_led_service(g_now + 200u) != OD_LED_IDLE);
        CHECK(g_pwm_waits > waits_before);
    }
}

/* Activation and stop verdicts, which the dispatcher turns into wire replies. */
static void test_verdicts(void)
{
    CASE("a payload whose mode is not run is refused");
    setup();
    set_pattern(0xFEu, 1u, 0u, 0u);
    g_pattern[0] = 0x00u;                            /* mode 0 = stop */
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 2);

    CASE("stop with no run is success, not an error");
    setup();
    CHECK(od_led_stop(0u, false) == 0);

    CASE("stop naming a different instance is refused and clears nothing");
    setup();
    set_pattern(0xFEu, 1u, 0u, 0u);
    CHECK(od_led_activate(3u, &g_pins, g_pattern, g_now) == 0);
    CHECK(od_led_stop(1u, true) == 2);
    CHECK(g_finished_calls == 0u);
    CHECK(od_led_stop(3u, true) == 0);
    CHECK(g_finished_calls == 1u);
    CHECK(g_finished_instance == 3u);
    CHECK(od_led_service(g_now) == OD_LED_IDLE);

    /* LED_STOP may arrive with no instance byte, so only the machine knows whose nibble to
     * clear. An adapter guessing instance 0 would clear the wrong one. */
    CASE("stop without an instance clears the RUNNING instance");
    setup();
    set_pattern(0xFEu, 1u, 0u, 0u);
    CHECK(od_led_activate(2u, &g_pins, g_pattern, g_now) == 0);
    CHECK(od_led_stop(0u, false) == 0);
    CHECK(g_finished_calls == 1u);
    CHECK(g_finished_instance == 2u);
    CHECK(od_led_service(g_now) == OD_LED_IDLE);

    CASE("stopping an idle runner reports nothing");
    setup();
    CHECK(od_led_stop(0u, false) == 0);
    CHECK(g_finished_calls == 0u);
}

/* An absent pin must not be driven -- a pattern may name colours the board has no LED for. */
static void test_absent_pins_are_not_driven(void)
{
    CASE("only configured pins are written");
    setup();
    set_pattern(0x00u, 1u, 0u, 0u);                  /* one group, one flash per loop */
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
    for (unsigned n = 0; n < 64u && pattern_running(); ++n) {
        (void)service_once();
    }
    /* g_pins has red only; green and blue are 0xFF and are counted nowhere. */
    CHECK(g_pin_writes > 0u);
    CHECK(g_pwm_waits == 3u * LED_PWM_WAITS_PER_FLASH);
}


/* The PWM ramp is the timing-sensitive part and the one a "simplification" would silently change.
 * Assert the level SEQUENCE, not just that writes happened. */
static void test_pwm_ramp_shape(void)
{
    /* Full red (0xE0 -> cr=7), brightness nibble 0 -> 1 pass. The authority's red thresholds are
     * 7,1,6,2,5,3,4 then off, so at cr=7 every comparison is true and the pass is seven highs
     * followed by the off write. */
    static const struct od_led_pins red_only = { 0x00u, 0xFFu, 0xFFu, 0x00u };
    uint8_t pattern[OD_LED_PATTERN_LEN];

    CASE("one flash at full red is seven highs then off");
    setup();
    memset(pattern, 0, sizeof pattern);
    pattern[0] = 0x01u;                              /* mode run, brightness 1 */
    od_led_flash_once(&red_only, 0xE0u, 1u);
    CHECK(g_write_n == 8u);
    for (unsigned i = 0; i < 7u; ++i) {
        CHECK(g_writes[i].pin == 0x00u);
        CHECK(g_writes[i].level);
    }
    CHECK(!g_writes[7].level);                       /* the trailing off */
    CHECK(g_pwm_waits == LED_PWM_WAITS_PER_FLASH);

    /* The inverted branch is a SEPARATE expression per slice, so it needs the same coverage: a
     * reordering there is invisible to a full-intensity check, where every threshold is true. */
    CASE("inversion is the exact complement at every intensity");
    {
        static const struct od_led_pins inverted = { 0x00u, 0xFFu, 0xFFu, 0x01u };
        static const struct { uint8_t cr; bool expect[7]; } cases[] = {
            { 0u, { false, false, false, false, false, false, false } },
            { 1u, { false, true , false, false, false, false, false } },
            { 2u, { false, true , false, true , false, false, false } },
            { 3u, { false, true , false, true , false, true , false } },
            { 4u, { false, true , false, true , false, true , true  } },
            { 5u, { false, true , false, true , true , true , true  } },
            { 6u, { false, true , true , true , true , true , true  } },
            { 7u, { true , true , true , true , true , true , true  } },
        };

        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
            setup();
            od_led_flash_once(&inverted, (uint8_t)(cases[c].cr << 5), 1u);
            CHECK(g_write_n == 8u);
            for (unsigned i = 0; i < 7u; ++i) {
                CHECK(g_writes[i].level == !cases[c].expect[i]);
            }
            CHECK(g_writes[7].level);                /* the trailing off, inverted */
        }
    }

    CASE("every red intensity follows the authority's threshold ORDER, not a ramp");
    {
        /* Slice thresholds are 7,1,6,2,5,3,4 -- deliberately NOT monotonic, which is what makes
         * intermediate levels look even. All eight intensities are pinned because a single level
         * leaves most thresholds indistinguishable, and that is how a reordering survives. */
        static const struct { uint8_t cr; bool expect[7]; } cases[] = {
            { 0u, { false, false, false, false, false, false, false } },
            { 1u, { false, true , false, false, false, false, false } },
            { 2u, { false, true , false, true , false, false, false } },
            { 3u, { false, true , false, true , false, true , false } },
            { 4u, { false, true , false, true , false, true , true  } },
            { 5u, { false, true , false, true , true , true , true  } },
            { 6u, { false, true , true , true , true , true , true  } },
            { 7u, { true , true , true , true , true , true , true  } },
        };

        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
            setup();
            od_led_flash_once(&red_only, (uint8_t)(cases[c].cr << 5), 1u);
            CHECK(g_write_n == 8u);
            for (unsigned i = 0; i < 7u; ++i) {
                CHECK(g_writes[i].level == cases[c].expect[i]);
            }
            CHECK(!g_writes[7].level);               /* trailing idle write */
        }
    }

    /* Green has the same thresholds as red but a SEPARATE expression per slice, so a mutation
     * there is invisible to the red tables. */
    CASE("the green channel is pinned independently at every intensity");
    {
        static const struct od_led_pins green_only = { 0xFFu, 0x01u, 0xFFu, 0x00u };
        static const struct { uint8_t cg; bool expect[7]; } cases[] = {
            { 0u, { false, false, false, false, false, false, false } },
            { 1u, { false, true , false, false, false, false, false } },
            { 2u, { false, true , false, true , false, false, false } },
            { 3u, { false, true , false, true , false, true , false } },
            { 4u, { false, true , false, true , false, true , true  } },
            { 5u, { false, true , false, true , true , true , true  } },
            { 6u, { false, true , true , true , true , true , true  } },
            { 7u, { true , true , true , true , true , true , true  } },
        };

        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
            setup();
            od_led_flash_once(&green_only, (uint8_t)(cases[c].cg << 2), 1u);
            CHECK(g_write_n == 8u);
            for (unsigned i = 0; i < 7u; ++i) {
                CHECK(g_writes[i].level == cases[c].expect[i]);
            }
        }
    }

    CASE("green inversion is the exact complement at every intensity");
    {
        static const struct od_led_pins green_inv = { 0xFFu, 0x01u, 0xFFu, 0x02u };
        static const struct { uint8_t cg; bool expect[7]; } cases[] = {
            { 0u, { false, false, false, false, false, false, false } },
            { 1u, { false, true , false, false, false, false, false } },
            { 2u, { false, true , false, true , false, false, false } },
            { 3u, { false, true , false, true , false, true , false } },
            { 4u, { false, true , false, true , false, true , true  } },
            { 5u, { false, true , false, true , true , true , true  } },
            { 6u, { false, true , true , true , true , true , true  } },
            { 7u, { true , true , true , true , true , true , true  } },
        };

        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
            setup();
            od_led_flash_once(&green_inv, (uint8_t)(cases[c].cg << 2), 1u);
            CHECK(g_write_n == 8u);
            for (unsigned i = 0; i < 7u; ++i) {
                CHECK(g_writes[i].level == !cases[c].expect[i]);
            }
            CHECK(g_writes[7].level);                /* trailing idle write, inverted */
        }
    }

    CASE("the blue channel uses its own 2-bit thresholds on its own three slices");
    {
        /* Blue is written on slices 0, 2 and 5 only, at thresholds 3,1,2 -- a different set and a
         * different cadence from red/green. Both polarities, all four intensities. */
        static const struct od_led_pins blue_only = { 0xFFu, 0xFFu, 0x0Bu, 0x00u };
        static const struct od_led_pins blue_inv  = { 0xFFu, 0xFFu, 0x0Bu, 0x04u };
        static const struct { uint8_t cb; bool expect[3]; } cases[] = {
            { 0u, { false, false, false } },
            { 1u, { false, true , false } },
            { 2u, { false, true , true  } },
            { 3u, { true , true , true  } },
        };

        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
            setup();
            od_led_flash_once(&blue_only, cases[c].cb, 1u);
            CHECK(g_write_n == 4u);                  /* three slices plus the trailing off */
            for (unsigned i = 0; i < 3u; ++i) {
                CHECK(g_writes[i].level == cases[c].expect[i]);
            }
            CHECK(!g_writes[3].level);

            setup();
            od_led_flash_once(&blue_inv, cases[c].cb, 1u);
            for (unsigned i = 0; i < 3u; ++i) {
                CHECK(g_writes[i].level == !cases[c].expect[i]);
            }
            CHECK(g_writes[3].level);
        }
    }

    CASE("brightness is the number of passes");
    setup();
    od_led_flash_once(&red_only, 0xE0u, 5u);
    CHECK(g_pwm_waits == 5u * LED_PWM_WAITS_PER_FLASH);
    CHECK(g_write_n == 5u * 8u);

    CASE("brightness 0 emits nothing");
    setup();
    od_led_flash_once(&red_only, 0xE0u, 0u);
    CHECK(g_write_n == 0u);
}

/* The three loops carry independent counts, colours and delays. A total-flash assertion is not
 * enough: permuting the counts between loops keeps the total. Assert the ORDER of the colours the
 * ramp emits, and give each loop a distinct delay so a swapped delay field shows up too. */
static void test_distinct_loop_fields(void)
{
    /* One pin per channel so the colour of each flash is identifiable from which pin moved. */
    static const struct od_led_pins rgb = { 0x00u, 0x01u, 0x02u, 0x00u };
    uint8_t pattern[OD_LED_PATTERN_LEN];
    uint8_t seen[8];
    unsigned seen_n = 0u;

    CASE("each loop uses its own count, colour and delay");
    setup();
    memset(pattern, 0, sizeof pattern);
    pattern[0] = 0x01u;                              /* mode run, brightness 1 */
    pattern[1] = 0xE0u;                              /* loop 1 colour: full red   */
    pattern[4] = 0x1Cu;                              /* loop 2 colour: full green */
    pattern[7] = 0x03u;                              /* loop 3 colour: full blue  */
    pattern[2] = (uint8_t)((1u << 4) | 1u);          /* loop 1: 1 flash, 100 ms */
    pattern[5] = (uint8_t)((2u << 4) | 2u);          /* loop 2: 2 flashes, 200 ms */
    pattern[8] = (uint8_t)((3u << 4) | 3u);          /* loop 3: 3 flashes, 300 ms */
    pattern[10] = 0x00u;                             /* one group */
    CHECK(od_led_activate(0u, &rgb, pattern, g_now) == 0);

    /* Identify each flash by which pin it drives HIGH -- the ramp writes red, green and blue on
     * every slice regardless of colour, so "the first pin written" identifies nothing. A full-red
     * flash raises pin 0 only, full green pin 1, full blue pin 2. */
    for (unsigned n = 0; n < 64u && pattern_running(); ++n) {
        const unsigned before = g_write_n;
        const uint32_t d = od_led_service(g_now);
        uint8_t high = 0xFFu;
        bool multiple = false;

        if (d == OD_LED_IDLE) {
            break;
        }
        for (unsigned w = before; w < g_write_n; ++w) {
            if (!g_writes[w].level) {
                continue;
            }
            if (high != 0xFFu && g_writes[w].pin != high) {
                multiple = true;
            }
            high = g_writes[w].pin;
        }
        if (high != 0xFFu && seen_n < sizeof seen) {
            CHECK(!multiple);                        /* one colour per flash */
            seen[seen_n] = high;
            /* The delay after a flash identifies which loop emitted it. */
            if (d == 100u) { CHECK(high == 0x00u); }
            if (d == 200u) { CHECK(high == 0x01u); }
            if (d == 300u) { CHECK(high == 0x02u); }
            ++seen_n;
        }
        g_now += d;
    }

    /* 1 red, then 2 green, then 3 blue -- in that order. A permutation of counts or colours
     * between loops changes this sequence even though the total stays six. */
    CHECK(seen_n == 6u);
    CHECK(seen[0] == 0x00u);
    CHECK(seen[1] == 0x01u);
    CHECK(seen[2] == 0x01u);
    CHECK(seen[3] == 0x02u);
    CHECK(seen[4] == 0x02u);
    CHECK(seen[5] == 0x02u);
}

/* Brightness comes from the pattern's high nibble, not from a caller argument. */
static void test_brightness_nibble_decoding(void)
{
    static const struct od_led_pins red_only = { 0x00u, 0xFFu, 0xFFu, 0x00u };
    uint8_t pattern[OD_LED_PATTERN_LEN];

    CASE("brightness is (high nibble + 1), so the range is 1..16");
    for (unsigned nib = 0; nib < 16u; ++nib) {
        setup();
        memset(pattern, 0, sizeof pattern);
        pattern[0] = (uint8_t)((nib << 4) | 0x01u);  /* mode run, brightness nibble */
        pattern[1] = 0xE0u;
        pattern[2] = 0x01u;                          /* loop 1: one flash, no delay */
        pattern[10] = 0x00u;                         /* one group */
        CHECK(od_led_activate(0u, &red_only, pattern, g_now) == 0);
        (void)od_led_service(g_now);                 /* the first flash */
        CHECK(g_pwm_waits == (nib + 1u) * LED_PWM_WAITS_PER_FLASH);
    }
}

/* Activating over a running pattern: the outgoing instance's LEDs are parked, and -- per the
 * authority -- its mode nibble is NOT cleared. */
static void test_activation_displaces(void)
{
    uint8_t pattern[OD_LED_PATTERN_LEN];

    static const struct od_led_pins all_three = { 0x00u, 0x01u, 0x02u, 0x00u };

    CASE("a new activation parks the outgoing instance");
    setup();
    set_pattern(0xFEu, 1u, 0u, 0u);
    CHECK(od_led_activate(1u, &all_three, g_pattern, g_now) == 0);
    CHECK(service_once() == 1u);

    memcpy(pattern, g_pattern, sizeof pattern);
    g_write_n = 0u;
    CHECK(od_led_activate(2u, &all_three, pattern, g_now) == 0);
    /* Every configured channel is parked at its idle level -- not merely "a write happened". */
    CHECK(g_write_n == 3u);
    CHECK(g_writes[0].pin == 0x00u && !g_writes[0].level);
    CHECK(g_writes[1].pin == 0x01u && !g_writes[1].level);
    CHECK(g_writes[2].pin == 0x02u && !g_writes[2].level);
    /* Not a completion -- the authority leaves the outgoing nibble alone. */
    CHECK(g_finished_calls == 0u);

    CASE("an inverted instance parks HIGH, not low");
    {
        static const struct od_led_pins inverted = { 0x00u, 0x01u, 0x02u, 0x07u };

        setup();
        set_pattern(0xFEu, 1u, 0u, 0u);
        CHECK(od_led_activate(1u, &inverted, g_pattern, g_now) == 0);
        CHECK(service_once() == 1u);
        g_write_n = 0u;
        CHECK(od_led_activate(2u, &inverted, g_pattern, g_now) == 0);
        CHECK(g_write_n == 3u);
        for (unsigned i = 0; i < 3u; ++i) {
            CHECK(g_writes[i].level);                /* idle for an inverted channel is high */
        }
    }

    CASE("stopping now names the NEW instance");
    CHECK(od_led_stop(0u, false) == 0);
    CHECK(g_finished_calls == 1u);
    CHECK(g_finished_instance == 2u);

    CASE("re-activating the same instance restarts it rather than stopping it");
    setup();
    set_pattern(0x00u, 1u, 0u, 0u);                  /* one group, one flash per loop */
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
    CHECK(service_once() == 1u);
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
    {
        const unsigned waits_before = g_pwm_waits;

        for (unsigned n = 0; n < 64u && pattern_running(); ++n) {
            (void)service_once();
        }
        CHECK(g_pwm_waits > waits_before);           /* it ran again */
    }
}

/* A late call advances exactly one step: the pattern slips, it does not fast-forward. */
static void test_late_call_advances_once(void)
{
    CASE("a very late call performs one step, not the number that fit");
    setup();
    set_pattern(0xFEu, 1u, 2u, 0u);                  /* 200 ms between flashes */
    CHECK(od_led_activate(0u, &g_pins, g_pattern, g_now) == 0);
    CHECK(od_led_service(g_now) == 200u);
    {
        const unsigned waits_before = g_pwm_waits;

        /* Ten periods late. */
        CHECK(od_led_service(g_now + 2000u) != OD_LED_IDLE);
        CHECK(g_pwm_waits - waits_before <= LED_PWM_WAITS_PER_FLASH);
    }
}

int main(void)
{
    test_all_zero_delays_yield();
    test_zero_delay_flashes_yield();
    test_group_repeat_sentinels();
    test_configured_delay_is_honoured();
    test_external_clear_stops();
    test_millisecond_wrap();
    test_verdicts();
    test_absent_pins_are_not_driven();
    test_pwm_ramp_shape();
    test_distinct_loop_fields();
    test_brightness_nibble_decoding();
    test_activation_displaces();
    test_late_call_advances_once();

    return OD_CHECK_REPORT_NONEMPTY("od_led", 100);
}
