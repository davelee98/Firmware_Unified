#include "od_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    char line[32];
    const uint8_t byte = 0xABu;
    int side_effect = 0;

    od_log_init();
    od_log_error("disabled %d", ++side_effect);
    od_log_warn("disabled %d", ++side_effect);
    od_log_info("disabled %d", ++side_effect);
    od_log_debug("disabled %d", ++side_effect);
    check(side_effect == 0, "capability-off macro arguments are not evaluated");

    _od_log(OD_LOG_INFO, "direct stub");
    od_log_raw("raw stub");
    od_log_flush();
    check(od_log_dropped_total() == 0u, "capability-off dropped total is zero");

    od_log_hex_line(line, sizeof(line), "x=", &byte, 1u);
    check(strcmp(line, "x=AB") == 0, "pure hex renderer remains available");

    printf("log off: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
