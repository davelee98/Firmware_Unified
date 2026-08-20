#include "od_hal_log.h"
#include "od_hal_time.h"
#include "od_log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef OD_LOG_TEXT_MAX
#error OD_LOG_TEXT_MAX must be defined by the test profile
#endif
#ifndef OD_LOG_RAW_TEXT_MAX
#error OD_LOG_RAW_TEXT_MAX must be defined by the test profile
#endif

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_record[512];
static size_t s_record_len;
static unsigned s_write_calls;
static unsigned s_flush_calls;
static unsigned s_checks;
static unsigned s_failures;
static int s_open;
static int s_saw_nul;
static uint32_t s_now_ms;
static uint32_t s_cycle;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        printf("FAIL: %s\n", what);
    }
}

void od_hal_log_open(void) { s_open = 1; }
bool od_hal_log_is_open(void) { return s_open != 0; }
uint32_t od_hal_uptime_ms(void) { return s_now_ms; }
void od_hal_delay_us(uint32_t us) { (void)us; }
uint32_t od_hal_log_cycle_count(void) { return s_cycle; }

void od_hal_log_write(char *record, size_t len)
{
    pthread_mutex_lock(&s_lock);
    ++s_write_calls;
    s_saw_nul = record != NULL && record[len] == '\0';
    s_record_len = len < sizeof(s_record) - 1u ? len : sizeof(s_record) - 1u;
    if (record != NULL && s_record_len > 0u) {
        memcpy(s_record, record, s_record_len);
    }
    s_record[s_record_len] = '\0';
    pthread_mutex_unlock(&s_lock);
}

void od_hal_log_flush(void) { ++s_flush_calls; }

static void clear_capture(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_record, 0, sizeof(s_record));
    s_record_len = 0u;
    s_write_calls = 0u;
    s_flush_calls = 0u;
    s_saw_nul = 0;
    pthread_mutex_unlock(&s_lock);
}

static void test_closed_and_open(void)
{
    s_open = 0;
    od_log_init();
    clear_capture();
    od_log_info("closed");
    od_log_raw("closed");
    od_log_flush();
    check(s_write_calls == 0u && s_flush_calls == 0u, "closed logger is inert");

    od_hal_log_open();
    od_log_init();
    clear_capture();
    od_log_info("open");
    check(s_write_calls == 1u, "open logger emits once");
}

static void test_prefixes_and_levels(void)
{
    static const struct {
        uint32_t now_ms;
        uint32_t cycle;
        int level;
        const char *expect;
    } cases[] = {
        {0u, 0u, OD_LOG_ERROR, "[0000.000|C0] E: x\r\n"},
        {999u, 7u, OD_LOG_WARN, "[0000.999|C7] W: x\r\n"},
        {1000u, 0u, OD_LOG_INFO, "[0001.000|C0] I: x\r\n"},
        {UINT32_MAX, 42u, OD_LOG_DEBUG, "[4294967.295|C42] D: x\r\n"},
        {12u, 3u, 99, "[0000.012|C3] I: x\r\n"},
    };
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        s_now_ms = cases[i].now_ms;
        s_cycle = cases[i].cycle;
        clear_capture();
        _od_log(cases[i].level, "x");
        check(s_write_calls == 1u, "normal record uses one HAL write");
        check(strcmp(s_record, cases[i].expect) == 0, "normal record bytes match");
        check(s_saw_nul, "normal record exposes a NUL after len");
    }
}

static void fill_text(char *buf, size_t n)
{
    size_t i;

    for (i = 0u; i < n; ++i) {
        buf[i] = (char)('a' + (i % 26u));
    }
    buf[n] = '\0';
}

static void test_truncation_boundaries(void)
{
    char text[400];
    size_t prefix_len;
    size_t offered;
    size_t targets[3];
    size_t i;

    s_now_ms = 0u;
    s_cycle = 0u;
    clear_capture();
    _od_log(OD_LOG_INFO, "%s", "");
    prefix_len = s_record_len - 2u;
    targets[0] = OD_LOG_TEXT_MAX - 1u;
    targets[1] = OD_LOG_TEXT_MAX;
    targets[2] = OD_LOG_TEXT_MAX + 1u;
    for (i = 0u; i < 3u; ++i) {
        offered = targets[i] > prefix_len ? targets[i] - prefix_len : 0u;
        fill_text(text, offered);
        clear_capture();
        _od_log(OD_LOG_INFO, "%s", text);
        check(s_record_len == (targets[i] > OD_LOG_TEXT_MAX ? OD_LOG_TEXT_MAX : targets[i]) + 2u,
              "normal cap boundary preserves expected length");
        check(s_record[s_record_len - 2u] == '\r' && s_record[s_record_len - 1u] == '\n',
              "normal cap boundary retains CRLF");
    }

    targets[0] = OD_LOG_RAW_TEXT_MAX - 1u;
    targets[1] = OD_LOG_RAW_TEXT_MAX;
    targets[2] = OD_LOG_RAW_TEXT_MAX + 1u;
    for (i = 0u; i < 3u; ++i) {
        fill_text(text, targets[i]);
        clear_capture();
        od_log_raw("%s", text);
        check(s_record_len == (targets[i] > OD_LOG_RAW_TEXT_MAX ? OD_LOG_RAW_TEXT_MAX : targets[i]),
              "raw cap boundary preserves expected length");
        check(s_record_len == 0u || s_record[s_record_len - 1u] != '\n',
              "raw output gets no automatic newline");
    }

    clear_capture();
    od_log_raw("%s", "");
    check(s_write_calls == 1u && s_record_len == 0u,
          "zero-length raw output still crosses the HAL once");
}

static void test_hex_renderer(void)
{
    uint8_t bytes[33];
    char buf[160];
    size_t i;

    for (i = 0u; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)i;
    }
    od_log_hex_line(buf, sizeof(buf), "L: ", bytes, 0u);
    check(strcmp(buf, "L: ") == 0, "zero-byte hex line contains label only");
    od_log_hex_line(buf, sizeof(buf), "L: ", bytes, 1u);
    check(strcmp(buf, "L: 00") == 0, "one-byte hex line matches");
    od_log_hex_line(buf, sizeof(buf), "", bytes, 32u);
    check(strlen(buf) == 95u && strstr(buf, " ...") == NULL, "32-byte hex line is complete");
    od_log_hex_line(buf, sizeof(buf), "", bytes, 33u);
    check(strlen(buf) == 99u && strcmp(buf + 95u, " ...") == 0,
          "33-byte hex line carries truncation marker");
}

static void test_flush_and_drop_count(void)
{
    clear_capture();
    od_log_flush();
    check(s_flush_calls == 1u, "flush reaches HAL exactly once");
    check(od_log_dropped_total() == 0u, "drop count is a real zero");
}

static void *thread_writer(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    unsigned i;

    for (i = 0u; i < 50u; ++i) {
        _od_log(OD_LOG_INFO, "thread %lu item %u", (unsigned long)id, i);
    }
    return NULL;
}

static void test_concurrent_records_are_single_writes(void)
{
    pthread_t threads[4];
    uintptr_t i;

    clear_capture();
    for (i = 0u; i < 4u; ++i) {
        (void)pthread_create(&threads[i], NULL, thread_writer, (void *)i);
    }
    for (i = 0u; i < 4u; ++i) {
        (void)pthread_join(threads[i], NULL);
    }
    check(s_write_calls == 200u, "concurrent records remain one HAL call each");
}

int main(void)
{
    test_closed_and_open();
    test_prefixes_and_levels();
    test_truncation_boundaries();
    test_hex_renderer();
    test_flush_and_drop_count();
    test_concurrent_records_are_single_writes();
    printf("log profile %u/%u: %u checks, %u failures\n",
           (unsigned)OD_LOG_TEXT_MAX, (unsigned)OD_LOG_RAW_TEXT_MAX, s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
