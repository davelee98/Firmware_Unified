/*
 * OpenDisplay's OWN log level -- deliberately NOT Zephyr's.
 *
 * WHY THIS EXISTS. This target logs with printf(), not LOG_INF()/LOG_DBG(), so
 * CONFIG_LOG_DEFAULT_LEVEL does nothing for OpenDisplay output. Raising it to DEBUG to get
 * more app detail instead turns on DEBUG for EVERY module -- the kernel emits a line per
 * k_mutex_lock/unlock and the USB stack one per udc_ep_enqueue, which on a CDC-ACM console
 * is self-feeding: logging generates USB traffic, which generates log lines. That was tried
 * on the nRF52840 and produced a console of pure os/udc spam with
 * "--- 883 messages dropped ---" burying the app's own output. Do not reach for
 * CONFIG_LOG_DEFAULT_LEVEL again; reach for this.
 *
 * LEVELS
 *   0  quiet  -- nothing (OD_LOW_POWER_QUIET territory)
 *   1  info   -- the normal boot/telemetry output. THE DEFAULT, and what every existing
 *                bare printf() in this target effectively already is.
 *   2  debug  -- bring-up detail: resolved pin mappings, panel command tracing.
 *
 * Existing bare printf() calls are intentionally left alone. Converting ~200 call sites
 * mid-bring-up would be churn with no diagnostic gain, since they are all "info" anyway.
 * New diagnostics should use od_dbg() so they can be compiled out.
 */
#ifndef OD_LOG_H
#define OD_LOG_H

#include <stdio.h>

#ifndef OD_LOG_LEVEL
#define OD_LOG_LEVEL 1
#endif

#define OD_LOG_LEVEL_QUIET 0
#define OD_LOG_LEVEL_INFO  1
#define OD_LOG_LEVEL_DEBUG 2

#if OD_LOG_LEVEL >= OD_LOG_LEVEL_INFO
#define od_info(...) printf(__VA_ARGS__)
#else
#define od_info(...) ((void)0)
#endif

/*
 * The (void)0 arms still have to *parse* their arguments, so a call that references a
 * variable only used for logging will not produce an unused-variable warning when the level
 * compiles it out -- and a typo inside od_dbg() is a build error at every level, not just at
 * OD_LOG_LEVEL=2.
 */
#if OD_LOG_LEVEL >= OD_LOG_LEVEL_DEBUG
#define od_dbg(...) printf(__VA_ARGS__)
#else
#define od_dbg(...) do { if (0) { printf(__VA_ARGS__); } } while (0)
#endif

#endif /* OD_LOG_H */
