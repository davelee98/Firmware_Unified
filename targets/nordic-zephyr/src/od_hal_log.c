/* Complete-record application log transport for Zephyr. */

#include "od_hal_log.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

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
	if (!s_open || record == NULL) {
		return;
	}

	/* Zephyr's deferred logger copies a mutable string into the log package before returning.
	 * A const cast would instead identify retained read-only storage. Width and precision are not
	 * supported for strings, so shared code supplies the terminating NUL beside len. */
	(void)len;
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
