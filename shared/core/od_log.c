#include "od_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if OD_CAP_LOG
#include "od_hal_log.h"
#include "od_hal_time.h"
#endif

#ifndef OD_LOG_RECORD_MAX
#define OD_LOG_RECORD_MAX 256u
#endif
#ifndef OD_LOG_TEXT_MAX
#define OD_LOG_TEXT_MAX 232u
#endif
#ifndef OD_LOG_RAW_TEXT_MAX
#define OD_LOG_RAW_TEXT_MAX OD_LOG_TEXT_MAX
#endif

#if defined(__cplusplus)
#define OD_LOG_STATIC_ASSERT(expr, msg) static_assert((expr), msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define OD_LOG_STATIC_ASSERT(expr, msg) _Static_assert((expr), msg)
#else
#define OD_LOG_STATIC_ASSERT_CAT_(a, b) a##b
#define OD_LOG_STATIC_ASSERT_CAT(a, b) OD_LOG_STATIC_ASSERT_CAT_(a, b)
#define OD_LOG_STATIC_ASSERT(expr, msg) \
    typedef char OD_LOG_STATIC_ASSERT_CAT(od_log_static_assert_, __LINE__)[(expr) ? 1 : -1]
#endif

OD_LOG_STATIC_ASSERT(OD_LOG_TEXT_MAX + 2u + 1u <= OD_LOG_RECORD_MAX,
                     "normal record plus CRLF and NUL must fit");
OD_LOG_STATIC_ASSERT(OD_LOG_RAW_TEXT_MAX + 1u <= OD_LOG_RECORD_MAX,
                     "raw record plus NUL must fit");

#if OD_CAP_LOG
static bool s_armed;
static const char s_level_chars[] = "EWID";
#endif

void od_log_init(void)
{
#if OD_CAP_LOG
    s_armed = od_hal_log_is_open();
#endif
}

uint32_t od_log_dropped_total(void)
{
    return 0u;
}

void _od_log(int level, const char *fmt, ...)
{
#if OD_CAP_LOG
    char record[OD_LOG_RECORD_MAX];
    uint32_t ms;
    int pos;
    size_t len;
    va_list args;

    if (!s_armed || fmt == NULL) {
        return;
    }
    if (level < OD_LOG_ERROR || level > OD_LOG_DEBUG) {
        level = OD_LOG_INFO;
    }

    ms = od_hal_uptime_ms();
    pos = snprintf(record, sizeof(record), "[%04lu.%03lu|C%lu] %c: ",
                   (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u),
                   (unsigned long)od_hal_log_cycle_count(), s_level_chars[level]);
    if (pos < 0 || (size_t)pos >= sizeof(record)) {
        return;
    }

    va_start(args, fmt);
    (void)vsnprintf(record + (size_t)pos, sizeof(record) - (size_t)pos, fmt, args);
    va_end(args);

    len = strlen(record);
    if (len > OD_LOG_TEXT_MAX) {
        len = OD_LOG_TEXT_MAX;
    }
    record[len++] = '\r';
    record[len++] = '\n';
    record[len] = '\0';
    od_hal_log_write(record, len);
#else
    (void)level;
    (void)fmt;
#endif
}

void od_log_raw(const char *fmt, ...)
{
#if OD_CAP_LOG
    char record[OD_LOG_RECORD_MAX];
    size_t len;
    va_list args;

    if (!s_armed || fmt == NULL) {
        return;
    }

    va_start(args, fmt);
    (void)vsnprintf(record, sizeof(record), fmt, args);
    va_end(args);

    len = strlen(record);
    if (len > OD_LOG_RAW_TEXT_MAX) {
        len = OD_LOG_RAW_TEXT_MAX;
    }
    record[len] = '\0';
    od_hal_log_write(record, len);
#else
    (void)fmt;
#endif
}

void od_log_flush(void)
{
#if OD_CAP_LOG
    if (s_armed) {
        od_hal_log_flush();
    }
#endif
}

void od_log_hex_line(char *buf, size_t buf_size, const char *label,
                     const uint8_t *data, uint16_t len)
{
    int pos;
    uint16_t dump_len;
    uint16_t i;

    if (buf == NULL || buf_size == 0u) {
        return;
    }
    if (label == NULL) {
        label = "";
    }

    pos = snprintf(buf, buf_size, "%s", label);
    if (pos < 0) {
        pos = 0;
        buf[0] = '\0';
    }
    dump_len = (len < 32u) ? len : 32u;
    for (i = 0u; i < dump_len && pos < (int)buf_size; ++i) {
        int n;

        if (data == NULL) {
            break;
        }
        n = snprintf(buf + pos, buf_size - (size_t)pos,
                     (i > 0u) ? " %02X" : "%02X", data[i]);
        if (n < 0) {
            break;
        }
        pos += n;
    }
    if (len > 32u && pos >= 0 && pos < (int)buf_size) {
        (void)snprintf(buf + pos, buf_size - (size_t)pos, " ...");
    }
}
