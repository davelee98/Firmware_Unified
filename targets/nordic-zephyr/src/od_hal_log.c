/* Complete-record application log transport for Zephyr. */

#include "od_hal_log.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

/* Mirrors od_log.c so the terminator rewrite can size a copy of the largest record that can
 * arrive: OD_LOG_TEXT_MAX of text plus CR LF. zephyr/CMakeLists.txt defines this for the target;
 * the fallback matches od_log.c's own. */
#ifndef OD_LOG_TEXT_MAX
#define OD_LOG_TEXT_MAX 232u
#endif

static bool s_open;

void od_hal_log_open(void)
{
	s_open = true;
}

bool od_hal_log_is_open(void)
{
	return s_open;
}

void od_hal_log_write(char *record, size_t len)
{
	char line[OD_LOG_TEXT_MAX + 2u];

	if (!s_open || record == NULL) {
		return;
	}

	/* od_log.c ends a complete record with CR LF, and Zephyr writes a CR ahead of every LF it
	 * forwards to the backend, so submitting the record unchanged puts CR CR LF on the wire. A
	 * terminal that reads the surplus CR as a column reset overprints the previous line, which
	 * is what the ESPHome web console does with the USB CDC ACM stream. Hand Zephyr the LF
	 * alone and let it own the terminator: LOG_PRINTK's contract is printk newline semantics,
	 * so it stays correct whichever way LOG_RAW's documented byte transparency is honoured.
	 * Unterminated data -- every od_log_raw() call -- keeps LOG_RAW, which promises to append
	 * nothing.
	 *
	 * od_hal_log.h forbids mutating or retaining the caller's buffer, hence the copy. Zephyr's
	 * deferred logger copies a mutable string into the log package before returning, which is
	 * both why that copy may live in this frame and why neither path may pass a const pointer
	 * (a const cast would instead identify retained read-only storage). Width and precision are
	 * not supported for strings, so both paths supply a NUL-terminated buffer. */
	if (len >= 2u && len <= sizeof(line) && record[len - 2u] == '\r' &&
	    record[len - 1u] == '\n') {
		memcpy(line, record, len - 2u);
		line[len - 2u] = '\n';
		line[len - 1u] = '\0';
		LOG_PRINTK("%s", line);
		return;
	}

	LOG_RAW("%s", record);
}

void od_hal_log_flush(void)
{
	if (s_open) {
		log_flush();
		k_msleep(5);
	}
}

uint32_t od_hal_log_cycle_count(void)
{
	return 0u;
}
