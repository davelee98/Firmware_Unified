/* Nordic log transport adapter over a modeled Zephyr logging backend.
 *
 * The bytes, not the call, are the contract: a complete record has to reach the wire with one
 * terminal CR LF and never CR CR LF, and data that carries no terminator must not acquire one.
 *
 * Two transports are modeled because Zephyr has two behaviours, and the adapter has to be right
 * under both. LOG_RAW is documented to append nothing and LOG_PRINTK to insert CR ahead of every
 * LF -- that is the DOCUMENTED model. In the SDK as shipped, LOG_PRINTK's raw-string marker is
 * consumed when the deferred message resolves its source to a name, so the output formatter
 * picks the CR-inserting callback for both macros -- that is the OBSERVED model, and it is what
 * puts CR CR LF on the CDC ACM wire. Every byte assertion below must hold in both, so the
 * adapter stays correct whichever way a later SDK settles it. */

#include "od_hal_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Modeled transport byte streams, wide enough for every record this test submits. */
static char s_wire_documented[2048];
static size_t s_wire_documented_len;
static char s_wire_observed[2048];
static size_t s_wire_observed_len;

static char s_queued[64];
static unsigned s_submits;
static unsigned s_raw_submits;
static unsigned s_printk_submits;
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

static void wire_put(char *wire, size_t *len, size_t cap, char c)
{
    if (*len < cap) {
        wire[(*len)++] = c;
    }
}

/* Zephyr's out_func: forwards every byte unchanged. */
static void emit_verbatim(char *wire, size_t *len, size_t cap, const char *record)
{
    size_t i;

    for (i = 0u; record[i] != '\0'; ++i) {
        wire_put(wire, len, cap, record[i]);
    }
}

/* Zephyr's cr_out_func: writes CR ahead of every LF it forwards. */
static void emit_cr_expanded(char *wire, size_t *len, size_t cap, const char *record)
{
    size_t i;

    for (i = 0u; record[i] != '\0'; ++i) {
        if (record[i] == '\n') {
            wire_put(wire, len, cap, '\r');
        }
        wire_put(wire, len, cap, record[i]);
    }
}

/* Deferred submission: the package is built from the record before the call returns, so the
 * fixture copies here for the same reason production Zephyr does. */
static void queue(char *record)
{
    ++s_submits;
    (void)snprintf(s_queued, sizeof(s_queued), "%s", record);
}

void fake_log_raw_submit(char *record)
{
    ++s_raw_submits;
    queue(record);
    emit_verbatim(s_wire_documented, &s_wire_documented_len, sizeof(s_wire_documented), record);
    emit_cr_expanded(s_wire_observed, &s_wire_observed_len, sizeof(s_wire_observed), record);
}

void fake_log_printk_submit(char *record)
{
    ++s_printk_submits;
    queue(record);
    emit_cr_expanded(s_wire_documented, &s_wire_documented_len, sizeof(s_wire_documented), record);
    emit_cr_expanded(s_wire_observed, &s_wire_observed_len, sizeof(s_wire_observed), record);
}

void log_flush(void) { ++s_flushes; }
void k_msleep(int32_t ms) { s_slept_ms += (uint32_t)ms; }

static int one_wire_equals(const char *wire, size_t wire_len, const char *expect)
{
    size_t len = strlen(expect);

    return wire_len == len && memcmp(wire, expect, len) == 0;
}

static int one_wire_contains(const char *wire, size_t wire_len, const char *needle)
{
    size_t len = strlen(needle);
    size_t i;

    if (len > wire_len) {
        return 0;
    }
    for (i = 0u; i + len <= wire_len; ++i) {
        if (memcmp(wire + i, needle, len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Both models or it does not hold. */
static int wire_equals(const char *expect)
{
    return one_wire_equals(s_wire_documented, s_wire_documented_len, expect) &&
           one_wire_equals(s_wire_observed, s_wire_observed_len, expect);
}

static int wire_contains(const char *needle)
{
    return one_wire_contains(s_wire_documented, s_wire_documented_len, needle) ||
           one_wire_contains(s_wire_observed, s_wire_observed_len, needle);
}

static void wire_reset(void)
{
    s_wire_documented_len = 0u;
    s_wire_observed_len = 0u;
}

/* Submit through a mutable frame-local copy, then destroy it, so anything the transport still
 * holds by reference shows up as corrupted wire bytes. */
static void write_then_clobber(const char *text)
{
    char scratch[OD_LOG_TEXT_MAX + 2u];
    size_t len = strlen(text);

    memcpy(scratch, text, len + 1u);
    od_hal_log_write(scratch, len);
    memset(scratch, 'X', sizeof(scratch));
}

int main(void)
{
    char transient[32] = "deferred record";
    char record[OD_LOG_TEXT_MAX + 3u];
    char expect[OD_LOG_TEXT_MAX + 4u];
    size_t i;

    check(!od_hal_log_is_open(), "Nordic log HAL starts closed");
    od_hal_log_write(transient, strlen(transient));
    check(s_submits == 0u, "closed Nordic log HAL is inert");
    check(wire_equals(""), "closed Nordic log HAL emits no bytes");

    od_hal_log_open();
    od_hal_log_open();
    check(od_hal_log_is_open(), "Nordic log HAL open is idempotent");
    od_hal_log_write(transient, strlen(transient));
    memset(transient, 'X', sizeof(transient));
    check(s_submits == 1u, "Nordic adapter submits one native record");
    check(strcmp(s_queued, "deferred record") == 0,
          "deferred package survives source-stack clobber");

    /* Unterminated data is raw by contract: no terminator is invented for it. */
    check(s_raw_submits == 1u && s_printk_submits == 0u,
          "unterminated data takes the raw path");
    check(wire_equals("deferred record"), "unterminated data reaches the wire verbatim");
    wire_reset();

    /* A complete record: od_log.c's CR LF must arrive as exactly one CR LF. */
    write_then_clobber("[0012.345|C0] I: hello\r\n");
    check(s_printk_submits == 1u, "complete record takes the printk path");
    check(wire_equals("[0012.345|C0] I: hello\r\n"),
          "complete record reaches the wire with one terminal CR LF");
    check(!wire_contains("\r\r\n"), "complete record produces no CR CR LF");
    wire_reset();

    /* The caller's buffer is an input, not scratch space: od_hal_log.h forbids mutating it. */
    {
        char caller[16];

        memcpy(caller, "abc\r\n", 6u);
        od_hal_log_write(caller, 5u);
        check(memcmp(caller, "abc\r\n", 6u) == 0, "adapter does not mutate the caller's record");
        check(wire_equals("abc\r\n"), "short complete record survives the rewrite");
        wire_reset();
    }

    /* The longest record od_log.c can emit still takes the rewrite path rather than falling
     * back to raw, which would put CR CR LF on the wire for the one case nearest the bound. */
    for (i = 0u; i < OD_LOG_TEXT_MAX; ++i) {
        record[i] = 'a';
    }
    record[OD_LOG_TEXT_MAX] = '\r';
    record[OD_LOG_TEXT_MAX + 1u] = '\n';
    record[OD_LOG_TEXT_MAX + 2u] = '\0';
    memcpy(expect, record, OD_LOG_TEXT_MAX + 3u);
    s_printk_submits = 0u;
    od_hal_log_write(record, OD_LOG_TEXT_MAX + 2u);
    check(s_printk_submits == 1u, "longest od_log.c record takes the printk path");
    check(wire_equals(expect), "longest record keeps one terminal CR LF");
    check(!wire_contains("\r\r\n"), "longest record produces no CR CR LF");
    wire_reset();

    /* Degenerate lengths must not index before the buffer. A lone LF is the one case the two
     * models disagree on, so it is asserted against each rather than through wire_equals(). */
    {
        char lone[2] = { '\n', '\0' };

        s_raw_submits = 0u;
        od_hal_log_write(lone, 1u);
        check(s_raw_submits == 1u, "a one-byte record cannot be a CR LF terminator");
        check(one_wire_equals(s_wire_documented, s_wire_documented_len, "\n"),
              "a one-byte record is forwarded raw");
        check(!wire_contains("\r\r"), "a one-byte record cannot double a carriage return");
        wire_reset();
    }

    od_hal_log_flush();
    check(s_flushes == 1u, "Nordic flush reaches Zephyr once");
    check(s_slept_ms == 5u, "Nordic flush retains five millisecond settlement");
    check(od_hal_log_cycle_count() == 0u, "Nordic cycle count is zero");

    printf("nordic log HAL: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
