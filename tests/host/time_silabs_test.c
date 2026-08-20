#include "od_hal_time.h"

#include "sl_status.h"

#include <stdint.h>
#include <stdio.h>

static uint64_t s_ticks;
static uint64_t s_converted_ticks;
static sl_status_t s_convert_status;
static uint32_t s_delay_us;
static unsigned s_delay_calls;
static unsigned s_checks;
static unsigned s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        printf("FAIL: %s\n", what);
    }
}

uint64_t sl_sleeptimer_get_tick_count64(void)
{
    return s_ticks;
}

sl_status_t sl_sleeptimer_tick64_to_ms(uint64_t ticks, uint64_t *ms)
{
    s_converted_ticks = ticks;
    if (s_convert_status == SL_STATUS_OK) {
        *ms = (ticks * UINT64_C(1000)) / UINT64_C(32768);
    }
    return s_convert_status;
}

void sl_udelay_wait(uint32_t us)
{
    ++s_delay_calls;
    s_delay_us = us;
}

static void reset_fake(void)
{
    s_ticks = 0u;
    s_converted_ticks = 0u;
    s_convert_status = SL_STATUS_OK;
    s_delay_us = 0u;
    s_delay_calls = 0u;
}

static void test_tick_rollover_is_not_uptime_rollover(void)
{
    uint32_t before;
    uint32_t after;

    reset_fake();
    s_ticks = (UINT64_C(1) << 32) - UINT64_C(32768);
    before = od_hal_uptime_ms();
    s_ticks = (UINT64_C(1) << 32) + UINT64_C(32768);
    after = od_hal_uptime_ms();

    check(after - before == 2000u, "uptime advances across the 32-bit hardware-tick rollover");
    check(s_converted_ticks > UINT32_MAX, "the wrapper passes the full 64-bit tick count");
}

static void test_millisecond_result_wraps_modulo_u32(void)
{
    const uint64_t full_ms = (UINT64_C(1) << 32) + UINT64_C(79);

    reset_fake();
    /* full_ms is divisible by 125, so this inverse 32.768 kHz conversion is exact. */
    s_ticks = (full_ms * UINT64_C(32768)) / UINT64_C(1000);

    check(od_hal_uptime_ms() == 79u,
          "uptime narrows modulo 2^32 only after conversion to milliseconds");
    check(s_converted_ticks == s_ticks, "millisecond wrap test converts the full tick value");
}

static void test_conversion_failure_returns_boot_origin(void)
{
    uint32_t result;

    reset_fake();
    s_ticks = UINT64_C(1234);
    s_convert_status = SL_STATUS_INVALID_PARAMETER;

    result = od_hal_uptime_ms();

    check(result == 0u, "pre-init conversion failure returns the boot-domain origin");
}

static void test_busy_wait_forwards_exactly(void)
{
    reset_fake();
    od_hal_delay_us(50u);
    check(s_delay_calls == 1u, "busy wait calls the SDK once");
    check(s_delay_us == 50u, "busy wait preserves the requested microseconds");
}

int main(void)
{
    test_tick_rollover_is_not_uptime_rollover();
    test_millisecond_result_wraps_modulo_u32();
    test_conversion_failure_returns_boot_origin();
    test_busy_wait_forwards_exactly();

    printf("silabs time: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
