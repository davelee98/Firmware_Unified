#include "od_log.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char level_chars[] = "EWID";

/* One complete application record is submitted as one native raw-log message. Zephyr owns the
 * queue, serialization and backend; OpenDisplay owns the record format. */
#define OD_LOG_BUF 256

void od_log_init(void)
{
	/* Zephyr initializes the logging core and configured backends during SYS_INIT. Kept as an
	 * API-compatible hook so shared startup code does not need a platform conditional. */
}

uint32_t od_log_dropped_total(void)
{
	/* Zephyr owns drop accounting internally and does not expose it through this compatibility
	 * API. Reporting a real 0 is better than fabricating a transport-level count. */
	return 0u;
}

__weak uint32_t od_log_cycle_count(void)
{
	return 0u;
}

static void od_emit(const char *text)
{
	/* LOG_RAW bypasses Zephyr's system-log envelope but still uses its deferred queue and sole
	 * configured backend. It appends nothing, so the application record remains byte-exact. */
	LOG_RAW("%s", text);
}

void _od_log(int level, const char *fmt, ...)
{
	char buf[OD_LOG_BUF];
	va_list args;
	uint32_t ms = k_uptime_get_32();
	int pos;

	if (level < 0 || level > OD_LOG_DEBUG) {
		level = OD_LOG_INFO;
	}

	pos = snprintf(buf, sizeof(buf), "[%04lu.%03lu|C%lu] %c: ",
		       (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u),
		       (unsigned long)od_log_cycle_count(), level_chars[level]);
	if (pos < 0 || pos >= (int)sizeof(buf)) {
		return;
	}

	va_start(args, fmt);
	(void)vsnprintf(buf + pos, sizeof(buf) - (size_t)pos, fmt, args);
	va_end(args);

	/* LOG_RAW does not append any characters. Terminate the application record here so UART,
	 * RTT and future Zephyr backends all carry the same wire format. */
	{
		size_t len = strlen(buf);

		if (len + 3u <= sizeof(buf)) {
			buf[len] = '\r';
			buf[len + 1u] = '\n';
			buf[len + 2u] = '\0';
		} else {
			buf[sizeof(buf) - 3u] = '\r';
			buf[sizeof(buf) - 2u] = '\n';
			buf[sizeof(buf) - 1u] = '\0';
		}
	}

	od_emit(buf);
}

void od_log_raw(const char *fmt, ...)
{
	char buf[OD_LOG_BUF];
	va_list args;

	va_start(args, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	/* No application header or newline by contract. */
	od_emit(buf);
}

void od_log_hex_line(char *buf, size_t bufSize, const char *label,
		     const uint8_t *data, uint16_t len)
{
	int pos = snprintf(buf, bufSize, "%s", label);
	int dumpLen = (len < 32u) ? (int)len : 32;

	if (pos < 0) {
		pos = 0;
		buf[0] = '\0';
	}
	for (int i = 0; i < dumpLen && pos < (int)bufSize; i++) {
		int n = snprintf(buf + pos, bufSize - (size_t)pos,
				 (i > 0) ? " %02X" : "%02X", data[i]);
		if (n < 0) {
			break;
		}
		pos += n;
	}
	if (len > 32u && pos < (int)bufSize) {
		(void)snprintf(buf + pos, bufSize - (size_t)pos, " ...");
	}
}

void od_log_flush(void)
{
	log_flush();

	/* The backend has accepted every queued record, but CDC still needs a USB poll to move its
	 * final packet to the host. Preserve the existing checkpoint settling budget. */
	k_msleep(5);
}
