/* BG22 LED runner yield contract, against the production state machine.
 *
 * `led_run_step()` is a `for (;;)` over phase transitions. A phase that neither emits nor waits
 * falls through and re-enters the loop, so a pattern whose delays are all zero can drive it
 * forever. This target is a superloop with no watchdog, and the LED pattern arrives over the
 * wire, so the loop has to be bounded by construction rather than by the caller.
 *
 * The bound asserted here is per SERVICE CALL, counted in yield-requiring actions -- LED
 * emissions and scheduled waits -- not in phase transitions. Zero-work transitions
 * (GROUP -> LOOP1 -> LOOP2 with empty loops) are legitimate inside one call; an unbounded number
 * of them is not.
 */

#include "opendisplay_led.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "opendisplay_runtime.h"

#include "em_cmu.h"
#include "em_gpio.h"
#include "sl_sleeptimer.h"
#include "sl_udelay.h"

#include <string.h>

#include "od_check.h"

/* ----------------------------------------------------------------- vendor driver fakes --- */

static unsigned g_pin_writes;       /* every GPIO level change the runner performs */
static unsigned g_pwm_waits;        /* od_flash_led issues exactly LED_PWM_WAITS_PER_FLASH */
static unsigned g_timer_starts;     /* scheduled waits */
static uint32_t g_last_timer_ms;
static sl_sleeptimer_timer_callback_t g_timer_cb;
static sl_sleeptimer_timer_handle_t *g_timer_handle;
static void *g_timer_data;
static bool g_timer_armed;

void CMU_ClockEnable(CMU_Clock_TypeDef clock, bool enable) { (void)clock; (void)enable; }

void GPIO_PinModeSet(GPIO_Port_TypeDef port, uint8_t pin, GPIO_Mode_TypeDef mode, unsigned out)
{ (void)port; (void)pin; (void)mode; (void)out; }

void GPIO_PinOutSet(GPIO_Port_TypeDef port, uint8_t pin)
{ (void)port; (void)pin; ++g_pin_writes; }

void GPIO_PinOutClear(GPIO_Port_TypeDef port, uint8_t pin)
{ (void)port; (void)pin; ++g_pin_writes; }

/* One flash is one PWM pass per brightness step, and each pass issues a fixed number of
 * micro-waits -- so counting these counts emissions without depending on colour or pin count. */
#define LED_PWM_WAITS_PER_FLASH 7u

void sl_udelay_wait(uint32_t us) { (void)us; ++g_pwm_waits; }

sl_status_t sl_sleeptimer_start_timer_ms(sl_sleeptimer_timer_handle_t *handle, uint32_t ms,
                                         sl_sleeptimer_timer_callback_t cb, void *data,
                                         uint8_t priority, uint16_t option_flags)
{
    (void)priority; (void)option_flags;
    ++g_timer_starts;
    g_last_timer_ms = ms;
    g_timer_cb = cb;
    g_timer_handle = handle;
    g_timer_data = data;
    g_timer_armed = true;
    return SL_STATUS_OK;
}

sl_status_t sl_sleeptimer_stop_timer(sl_sleeptimer_timer_handle_t *handle)
{
    (void)handle;
    g_timer_armed = false;
    return SL_STATUS_OK;
}

/* Fire whatever wait the runner scheduled, as the sleeptimer ISR would. */
static void elapse_scheduled_delay(void)
{
    if (g_timer_armed && g_timer_cb != NULL) {
        g_timer_armed = false;
        g_timer_cb(g_timer_handle, g_timer_data);
    }
}

/* ------------------------------------------------------------------------ config fake --- */

static struct GlobalConfig g_cfg;

const struct GlobalConfig *opendisplay_get_global_config(void) { return &g_cfg; }

/* The frame body the runner parses into its phase state; see led_load_config(). Staged apart from
 * led->reserved because activate copies into that, and a copy onto itself is not a copy. */
static uint8_t g_payload[12];

static void set_pattern(uint8_t group_repeats_raw, uint8_t loopcnt, uint8_t loopdelay,
                        uint8_t ildelay)
{
    uint8_t *r = g_payload;

    memset(r, 0, 12);
    r[0] = 0x01u;                                    /* mode 1 = run, brightness nibble 0 */
    r[1] = r[4] = r[7] = 0x01u;                      /* colours */
    r[2] = r[5] = r[8] = (uint8_t)((loopdelay << 4) | (loopcnt & 0x0Fu));
    r[3] = r[6] = r[9] = ildelay;
    r[10] = group_repeats_raw;
}

static void setup(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.loaded = true;
    g_cfg.led_count = 1u;
    g_cfg.leds[0].led_1_r = 0x00u;                   /* port A pin 0 */
    g_cfg.leds[0].led_2_g = GPIO_PIN_UNUSED;
    g_cfg.leds[0].led_3_b = GPIO_PIN_UNUSED;
    g_cfg.leds[0].led_4 = GPIO_PIN_UNUSED;
    g_pin_writes = 0u;
    g_pwm_waits = 0u;
    g_timer_starts = 0u;
    g_last_timer_ms = 0u;
    g_timer_cb = NULL;
    g_timer_armed = false;
    opendisplay_led_stop(0, false);
}

/* The runner keeps its own state private; the mode nibble in reserved[0] is what it re-reads on
 * every step and what a host sees, so that is the liveness signal asserted here. Completion
 * clears it. */
static bool pattern_running(void)
{
    return (g_cfg.leds[0].reserved[0] & 0x0Fu) == 0x01u;
}

/* One service call is bounded when it schedules exactly one wait: the runner cannot have looped
 * past a scheduled wait without returning, because scheduling is always followed by a return. */
static unsigned service_and_count_waits(void)
{
    const unsigned before = g_timer_starts;

    elapse_scheduled_delay();
    opendisplay_led_process();
    return g_timer_starts - before;
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
        CHECK(opendisplay_led_activate(0, g_payload, 12u) == 0);
        CHECK(pattern_running());
        CHECK(g_timer_starts == 1u);                 /* activate itself yielded */
        CHECK(g_last_timer_ms == 1u);                /* the minimum step delay */

        for (unsigned n = 0; n < 32u; ++n) {
            CHECK(service_and_count_waits() == 1u);
        }
        /* Still running: an endless pattern must not be terminated by the floor. */
        CHECK(pattern_running());
    }
}

/* A flashing pattern with no configured inter-flash delay is the other non-yielding path. */
static void test_zero_delay_flashes_yield(void)
{
    const unsigned brightness = 1u;                  /* set_pattern leaves reserved[0]'s high nibble 0 */

    CASE("a zero-delay flashing pattern emits at most one flash per service call");
    setup();
    set_pattern(0xFEu, 3u, 0u, 0u);                  /* three flashes per loop, no delays */
    CHECK(opendisplay_led_activate(0, g_payload, 12u) == 0);
    CHECK(g_pwm_waits == LED_PWM_WAITS_PER_FLASH * brightness);   /* activate emitted one flash */
    CHECK(g_timer_starts == 1u);
    CHECK(g_last_timer_ms == 1u);

    for (unsigned n = 0; n < 24u; ++n) {
        const unsigned waits_before = g_pwm_waits;

        CHECK(service_and_count_waits() == 1u);
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
    const unsigned flashes_per_group = 3u;               /* one per loop, loopcnt 1 each */

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        CASE("group_repeats: 0xFE and 0xFF are endless, a finite count runs count+1 groups");
        setup();
        set_pattern(cases[i].raw, 1u, 0u, 0u);
        CHECK(opendisplay_led_activate(0, g_payload, 12u) == 0);

        for (unsigned n = 0; n < 256u && pattern_running(); ++n) {
            (void)service_and_count_waits();
        }
        CHECK(pattern_running() == cases[i].endless);

        if (cases[i].endless) {
            continue;
        }
        CHECK(g_pwm_waits ==
              cases[i].groups * flashes_per_group * LED_PWM_WAITS_PER_FLASH * brightness);
        CHECK(g_cfg.leds[0].reserved[0] == 0x00u);
    }
}

/* A configured delay is still honoured: the runner must not run ahead of an unelapsed wait. */
static void test_configured_delay_is_honoured(void)
{
    CASE("a configured delay holds the runner until it elapses");
    setup();
    set_pattern(0xFEu, 1u, 2u, 0u);                  /* loop delay 2 * 100 ms */
    CHECK(opendisplay_led_activate(0, g_payload, 12u) == 0);
    CHECK(g_last_timer_ms == 200u);

    {
        const unsigned pins_before = g_pin_writes;
        const unsigned starts_before = g_timer_starts;

        opendisplay_led_process();                   /* without elapsing the wait */
        CHECK(g_pin_writes == pins_before);
        CHECK(g_timer_starts == starts_before);
    }
}

int main(void)
{
    test_all_zero_delays_yield();
    test_zero_delay_flashes_yield();
    test_group_repeat_sentinels();
    test_configured_delay_is_honoured();

    return OD_CHECK_REPORT_NONEMPTY("silabs_led", 100);
}
