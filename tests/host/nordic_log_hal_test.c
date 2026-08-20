#include "od_hal_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char s_queued[64];
static unsigned s_submits;
static unsigned s_flushes;
static uint32_t s_slept_ms;
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

void fake_log_raw_submit(char *record)
{
    ++s_submits;
    (void)snprintf(s_queued, sizeof(s_queued), "%s", record);
}

void log_flush(void) { ++s_flushes; }
void k_msleep(int32_t ms) { s_slept_ms += (uint32_t)ms; }

int main(void)
{
    char transient[32] = "deferred record";

    check(!od_hal_log_is_open(), "Nordic log HAL starts closed");
    od_hal_log_write(transient, strlen(transient));
    check(s_submits == 0u, "closed Nordic log HAL is inert");

    od_hal_log_open();
    od_hal_log_open();
    check(od_hal_log_is_open(), "Nordic log HAL open is idempotent");
    od_hal_log_write(transient, strlen(transient));
    memset(transient, 'X', sizeof(transient));
    check(s_submits == 1u, "Nordic adapter submits one native record");
    check(strcmp(s_queued, "deferred record") == 0,
          "deferred package survives source-stack clobber");

    od_hal_log_flush();
    check(s_flushes == 1u, "Nordic flush reaches Zephyr once");
    check(s_slept_ms == 5u, "Nordic flush retains five millisecond settlement");
    check(od_hal_log_cycle_count() == 0u, "Nordic cycle count is zero");

    printf("nordic log HAL: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
