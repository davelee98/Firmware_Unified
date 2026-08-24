/* The BG22 LED adapter's scheduler, against the production source.
 *
 * led_test.c covers the shared machine; nothing covered the three adapters that turn its returned
 * delay into a timer. BG22 is the one worth binding: it is a superloop with no watchdog, so an
 * adapter that fails to re-arm does not merely slip -- the pattern stops and nothing notices.
 *
 * What it does NOT cover: a zero delay. od_led_service() never returns one (od_led.h), so the
 * adapter carries no such branch and this suite cannot manufacture the case.
 */

#include "opendisplay_led.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "opendisplay_runtime.h"

#include "em_cmu.h"
#include "em_gpio.h"
#include "sl_sleeptimer.h"

#include "od_check.h"

#include <string.h>

/* ------------------------------------------------------------------ vendor fakes --- */

static unsigned g_timer_starts;
static unsigned g_timer_stops;
static uint32_t g_last_timer_ms;
static sl_sleeptimer_timer_callback_t g_cb;
static sl_sleeptimer_timer_handle_t *g_handle;
static bool g_armed;
static uint32_t g_now = 1000u;

void CMU_ClockEnable(CMU_Clock_TypeDef clock, bool enable) { (void)clock; (void)enable; }
void GPIO_PinModeSet(GPIO_Port_TypeDef p, uint8_t n, GPIO_Mode_TypeDef m, unsigned o)
{ (void)p; (void)n; (void)m; (void)o; }
void GPIO_PinOutSet(GPIO_Port_TypeDef p, uint8_t n) { (void)p; (void)n; }
void GPIO_PinOutClear(GPIO_Port_TypeDef p, uint8_t n) { (void)p; (void)n; }
void sl_udelay_wait(uint32_t us) { (void)us; }

uint32_t od_hal_uptime_ms(void) { return g_now; }
void od_hal_delay_us(uint32_t us) { (void)us; }

sl_status_t sl_sleeptimer_start_timer_ms(sl_sleeptimer_timer_handle_t *handle, uint32_t ms,
                                         sl_sleeptimer_timer_callback_t cb, void *data,
                                         uint8_t priority, uint16_t option_flags)
{
    (void)data; (void)priority; (void)option_flags;
    ++g_timer_starts;
    g_last_timer_ms = ms;
    g_cb = cb;
    g_handle = handle;
    g_armed = true;
    return SL_STATUS_OK;
}

sl_status_t sl_sleeptimer_stop_timer(sl_sleeptimer_timer_handle_t *handle)
{
    (void)handle;
    ++g_timer_stops;
    g_armed = false;
    return SL_STATUS_OK;
}

/* Fire the armed timer, as the sleeptimer ISR would, and advance the clock to its deadline. */
static void elapse(void)
{
    if (g_armed && g_cb != NULL) {
        g_now += g_last_timer_ms;
        g_armed = false;
        g_cb(g_handle, NULL);
    }
}

/* ---------------------------------------------------------------------- config --- */

static struct GlobalConfig g_cfg;

const struct GlobalConfig *opendisplay_get_global_config(void) { return &g_cfg; }

static uint8_t g_payload[12];

static void setup(uint8_t group_repeats_raw, uint8_t loopcnt, uint8_t loopdelay)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.loaded = true;
    g_cfg.led_count = 2u;
    for (unsigned i = 0; i < 2u; ++i) {
        g_cfg.leds[i].led_1_r = 0x00u;
        g_cfg.leds[i].led_2_g = GPIO_PIN_UNUSED;
        g_cfg.leds[i].led_3_b = GPIO_PIN_UNUSED;
        g_cfg.leds[i].led_4 = GPIO_PIN_UNUSED;
    }
    memset(g_payload, 0, sizeof g_payload);
    g_payload[0] = 0x01u;
    g_payload[1] = g_payload[4] = g_payload[7] = 0x01u;
    g_payload[2] = g_payload[5] = g_payload[8] = (uint8_t)((loopdelay << 4) | (loopcnt & 0x0Fu));
    g_payload[10] = group_repeats_raw;

    g_timer_starts = 0u;
    g_timer_stops = 0u;
    g_last_timer_ms = 0u;
    g_cb = NULL;
    g_armed = false;
    g_now = 1000u;
    (void)opendisplay_led_stop(0u, false);
}

/* ----------------------------------------------------------------------- cases --- */

static void test_arms_and_rearms(void)
{
    CASE("activate arms the timer for the delay the machine asked for");
    setup(0xFEu, 1u, 0u);
    CHECK(opendisplay_led_activate(0u, g_payload, 12u) == 0);
    CHECK(g_timer_starts == 1u);
    CHECK(g_last_timer_ms == 1u);                    /* the minimum step */

    CASE("process before the timer fires does nothing");
    {
        const unsigned starts = g_timer_starts;

        opendisplay_led_process();
        CHECK(g_timer_starts == starts);
    }

    CASE("each expiry re-arms, so an endless pattern keeps running");
    for (unsigned n = 0; n < 16u; ++n) {
        const unsigned starts = g_timer_starts;

        elapse();
        opendisplay_led_process();
        CHECK(g_timer_starts == starts + 1u);
    }
    CHECK((g_cfg.leds[0].reserved[0] & 0x0Fu) == 1u);
}

static void test_configured_delay_reaches_the_timer(void)
{
    CASE("a configured loop delay is armed in milliseconds, not delay units");
    setup(0xFEu, 1u, 3u);                            /* 3 * 100 ms */
    CHECK(opendisplay_led_activate(0u, g_payload, 12u) == 0);
    CHECK(g_last_timer_ms == 300u);
}

static void test_completion_stops_the_timer(void)
{
    CASE("a finite pattern stops the timer and clears the mode nibble");
    setup(0x00u, 1u, 0u);                            /* one group */
    CHECK(opendisplay_led_activate(0u, g_payload, 12u) == 0);
    for (unsigned n = 0; n < 64u && (g_cfg.leds[0].reserved[0] & 0x0Fu) == 1u; ++n) {
        elapse();
        opendisplay_led_process();
    }
    CHECK((g_cfg.leds[0].reserved[0] & 0x0Fu) == 0u);

    CASE("process after completion is inert");
    {
        const unsigned starts = g_timer_starts;

        opendisplay_led_process();
        opendisplay_led_process();
        CHECK(g_timer_starts == starts);
    }
}

static void test_stop_disarms(void)
{
    CASE("stop disarms the timer and clears the nibble");
    setup(0xFEu, 1u, 0u);
    CHECK(opendisplay_led_activate(0u, g_payload, 12u) == 0);
    CHECK(opendisplay_led_stop(0u, true) == 0);
    CHECK(!g_armed);
    CHECK((g_cfg.leds[0].reserved[0] & 0x0Fu) == 0u);

    CASE("stop naming another instance is refused and leaves the run armed");
    setup(0xFEu, 1u, 0u);
    CHECK(opendisplay_led_activate(1u, g_payload, 12u) == 0);
    CHECK(opendisplay_led_stop(0u, true) == 2);
    CHECK(g_armed);
}

static void test_bad_instance(void)
{
    CASE("an out-of-range instance is refused without arming anything");
    setup(0xFEu, 1u, 0u);
    CHECK(opendisplay_led_activate(9u, g_payload, 12u) == 2);
    CHECK(g_timer_starts == 0u);
}

static void test_non_run_payload_while_idle(void)
{
    CASE("an idle non-run activation preserves the selected and unrelated mode bytes");
    setup(0xFEu, 1u, 0u);
    g_cfg.leds[0].reserved[0] = 0x55u;                /* unrelated inactive instance */
    g_payload[0] = 0xA2u;                            /* brightness 10, mode 2: not run */

    CHECK(opendisplay_led_activate(1u, g_payload, sizeof g_payload) == 0);
    CHECK(g_cfg.leds[1].reserved[0] == 0xA2u);        /* copied payload is not cleared */
    CHECK(g_cfg.leds[0].reserved[0] == 0x55u);        /* no guessed-instance clear */
    CHECK(g_timer_starts == 0u);
    CHECK(!g_armed);
}

int main(void)
{
    test_arms_and_rearms();
    test_configured_delay_reaches_the_timer();
    test_completion_stops_the_timer();
    test_stop_disarms();
    test_bad_instance();
    test_non_run_payload_while_idle();

    return OD_CHECK_REPORT_NONEMPTY("silabs_led_adapter", 25);
}
