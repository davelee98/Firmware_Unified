#include <stdarg.h>

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}
