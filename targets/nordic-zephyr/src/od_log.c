#include "od_log.h"

#include <zephyr/kernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Zephyr port of Firmware/src/od_log.cpp. The record format and the level semantics are
 * identical; the delivery path is not, and the difference is deliberate -- see od_log.h.
 */

static const char level_chars[] = "EWID";

/*
 * Serialises whole records against each other.
 *
 * NOT a hang guard and not a correctness requirement -- each record is emitted with a SINGLE
 * printf of an already-assembled buffer, so a record can never be interleaved mid-way the way
 * a run of printf("%s") calls could be. What this buys is ordering between threads: BLE
 * callbacks, the display path and main() all log, and without it two records can reach the
 * console interleaved at the driver level.
 *
 * A failed take does NOT drop the record. Losing log lines to contention is a worse outcome
 * than a rare interleave, and unlike the Arduino target there is no TX-space reservation here
 * that a lock could protect.
 */
static struct k_mutex s_lock;
static bool s_lock_ready;

/* Bounded so logging from an ISR-adjacent or high-priority context cannot stall on a lower
 * priority thread holding the lock. 20 ms matches the Arduino target's budget. */
#define OD_LOG_LOCK_MS 20

/* Longest record handed to the console; matches the ESP32 source's OD_LOG_MAX_TEXT budget
 * via the 256-byte assembly buffer below. The longest thing emitted is a hex line (32 bytes
 * rendered as "%02X " plus a label), which is comfortably inside it. */
#define OD_LOG_BUF 256

void od_log_init(void)
{
	k_mutex_init(&s_lock);
	s_lock_ready = true;
}

uint32_t od_log_dropped_total(void)
{
	/* See od_log.h: Zephyr's console exposes no TX-room query, so there is nothing to
	 * count. Reporting a real 0 is better than a fabricated number. */
	return 0u;
}

__weak uint32_t od_log_cycle_count(void)
{
	return 0u;
}

static void od_emit(const char *text)
{
	bool locked = false;

	/*
	 * k_is_in_isr() is load-bearing: k_mutex_lock() with a non-zero timeout is illegal in
	 * ISR context and would fault rather than log. Anything logging from an ISR skips the
	 * lock and prints unserialised, which is the right trade for a diagnostic.
	 */
	if (s_lock_ready && !k_is_in_isr()) {
		locked = (k_mutex_lock(&s_lock, K_MSEC(OD_LOG_LOCK_MS)) == 0);
	}

	/*
	 * printf(), NOT od_log_raw(). od_log_raw() calls od_emit(), so routing this through it
	 * is unbounded recursion -- od_emit -> od_log_raw -> od_emit -> ... at 256 bytes of
	 * stack per frame. It got that way because the mechanical printf->od_log_* conversion
	 * was run across src/ INCLUDING THIS FILE, which is the one file it must never touch.
	 *
	 * ONE call, not a run of writes: the buffer is already the complete record, and printf
	 * is the same path every call site in this target used before the conversion, so
	 * delivery behaviour is unchanged.
	 */
	printf("%s", text);

	if (locked) {
		k_mutex_unlock(&s_lock);
	}
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

	/*
	 * The line ending is appended here rather than being carried in every format string,
	 * which is what lets call sites drop their trailing "\r\n". CRLF, not LF: these logs are
	 * read on terminals that do not translate, and the previous bare printf() calls in this
	 * target already used CRLF.
	 */
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

	/* No header and no newline by contract -- this is how a caller builds one line out of
	 * several calls (progress dots). Adding either would corrupt that. */
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
	fflush(stdout);

	/*
	 * Settling pause, matching the ESP32 source's rationale: fflush() returns once the
	 * driver has the bytes, which is not the same as the host having seen them -- on a CDC
	 * console the transfer still has to be polled off the device. od_log_flush() is for
	 * boot/wake checkpoints and pre-power-cut, where losing the last line costs the most and
	 * 5 ms costs nothing.
	 */
	k_msleep(5);
}
