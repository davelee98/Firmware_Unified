/* Log lines, severities and the [WDT] prefix are the canonical ones from
 * Firmware/src/watchdog_nrf.cpp, and this file is deliberately the same module line for line as
 * targets/nordic-zephyr/src/od_watchdog_app.c apart from the clock and the absence of a lock.
 * KEEP THE PAIR IN STEP. They are two copies because shared/ has no logging HAL and od_watchdog
 * therefore cannot emit the boot report itself; the day one lands, both collapse into it.
 *
 * The reason NAMES are the portable OD_HAL_WDT_RESET_* set rather than this chip's own, which is
 * the point of the promotion: the same vocabulary now appears in an ESP32 log and an nRF log.
 * The raw esp_reset_reason() value is still logged separately by setup().
 *
 * NO LOCK, unlike the Nordic copy. Every entry point here is reached from the loop task -- the
 * one setup() runs in -- which is also the only task ESP-IDF's TWDT feed will accept.
 */
#include "od_watchdog_app.h"

#include "od_hal_time.h"
#include "od_log.h"
#include "od_watchdog.h"

#include <stdio.h>

static struct od_watchdog s_wdt;

/* Decodes the OD_HAL_WDT_RESET_* bitmask. A bitmask, so print every set bit rather than the
 * first match -- even where this target sets only one, because the decoder is the same one the
 * Nordic copy uses and the two must not disagree about what a given mask reads as. */
static void logResetReason(uint32_t r)
{
    static const struct { uint32_t msk; const char *name; } kinds[] = {
        { OD_HAL_WDT_RESET_POWER_ON, "POWERON"  },
        { OD_HAL_WDT_RESET_WATCHDOG, "DOG"      },
        { OD_HAL_WDT_RESET_PIN,      "RESETPIN" },
        { OD_HAL_WDT_RESET_SOFTWARE, "SREQ"     },
        { OD_HAL_WDT_RESET_LOCKUP,   "LOCKUP"   },
        { OD_HAL_WDT_RESET_BROWNOUT, "BROWNOUT" },
        { OD_HAL_WDT_RESET_OTHER,    "OTHER"    },
    };
    char buf[80];
    size_t n = 0;
    uint32_t seen = 0;

    if (r == OD_HAL_WDT_RESET_UNKNOWN) {
        od_log_info("[WDT] reset reason: UNKNOWN (0x00)");
        return;
    }

    buf[0] = '\0';
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if ((r & kinds[i].msk) == 0u) {
            continue;
        }
        seen |= kinds[i].msk;
        int w = snprintf(buf + n, sizeof(buf) - n, "%s%s", n ? "|" : "", kinds[i].name);
        /* snprintf returns what it WOULD have written; clamp so a truncating write cannot
         * push n past the buffer. */
        if (w <= 0) {
            break;
        }
        n += (size_t)w;
        if (n >= sizeof(buf) - 1u) {
            n = sizeof(buf) - 1u;
            break;
        }
    }
    if ((r & ~seen) != 0u) {
        (void)snprintf(buf + n, sizeof(buf) - n, "%sUNMAPPED", n ? "|" : "");
    }
    od_log_info("[WDT] reset reason: %s (0x%02X)", buf, (unsigned)r);
}

void od_watchdog_app_boot(void)
{
    struct od_watchdog_boot_report report;

    od_watchdog_boot_init(&s_wdt, od_hal_uptime_ms(), &report);

    logResetReason(report.reset_reason);

    if (!report.retained_readable) {
        od_log_warn("[WDT] retained store unreadable - breadcrumb and strike count unavailable");
    } else if (report.breadcrumb_valid) {
        od_log_info("[WDT] breadcrumb from previous run: phase=%s (%u) strikes=%u",
                    od_watchdog_phase_name(report.prev_phase),
                    (unsigned)report.prev_phase, (unsigned)report.strikes);
    } else {
        od_log_info("[WDT] no retained breadcrumb (cold start or first boot)");
        if (!report.retained_writable) {
            od_log_warn("[WDT] retained store unwritable - breadcrumbs disabled");
        }
    }

    if (report.was_watchdog) {
        od_log_warn("[WDT] previous boot ended in a watchdog reset (strike %u/%u)",
                    (unsigned)report.strikes, (unsigned)OD_WDT_SAFE_MODE_STRIKES);
    }

    if (report.safe_mode) {
        od_log_error("[WDT] SAFE MODE: %u consecutive watchdog resets - skipping ALL panel "
                     "work this boot so the device stays reachable over BLE. "
                     "Clears after %lu s of healthy uptime.",
                     (unsigned)report.strikes, (unsigned long)(OD_WDT_HEALTHY_MS / 1000UL));
    }
}

void od_watchdog_app_arm(void)
{
    enum od_hal_wdt_arm_result rc = od_watchdog_arm(&s_wdt);

    switch (rc) {
    case OD_HAL_WDT_ARM_OK:
        od_log_info("[WDT] armed: %us", (unsigned)OD_WDT_TIMEOUT_S);
        break;
    case OD_HAL_WDT_ARM_INHERITED:
        /* Not the nRF brick case: IDF starts the TWDT before app_main, so this is the
         * ordinary path. hal/od_hal_wdt.c says what adopting it changes. */
        od_log_info("[WDT] armed: %us (adopted the TWDT IDF had already started)",
                    (unsigned)OD_WDT_TIMEOUT_S);
        break;
    case OD_HAL_WDT_ARM_DISABLED:
        od_log_warn("[WDT] disabled at build time (OD_WDT_TIMEOUT_S=0)");
        break;
    case OD_HAL_WDT_ARM_UNSUPPORTED:
        od_log_warn("[WDT] task watchdog compiled out (CONFIG_ESP_TASK_WDT_EN=n)");
        break;
    default:
        od_log_error("[WDT] arm FAILED - this boot has no watchdog");
        break;
    }
}

void od_watchdog_app_service(void)
{
    const bool pending_before = s_wdt.strikes_to_clear;

    od_watchdog_feed(&s_wdt, od_hal_uptime_ms());

    /* Only on the transition: the uptime rule runs on every feed, so an unconditional line
     * here would be one per loop pass. */
    if (pending_before && !s_wdt.strikes_to_clear) {
        od_log_info("[WDT] %lu s of healthy uptime - strike counter cleared",
                    (unsigned long)(OD_WDT_HEALTHY_MS / 1000UL));
    }
}

bool od_watchdog_app_safe_mode(void)
{
    return od_watchdog_in_safe_mode(&s_wdt);
}

void od_watchdog_app_phase(uint8_t phase)
{
    static bool failLogged;
    const bool failed_before = s_wdt.write_failed;

    od_watchdog_breadcrumb(&s_wdt, phase);

    /* Latched: breadcrumb sites sit on per-frame paths, so this must warn once and then stay
     * quiet. A silently dead breadcrumb is worse than none -- the boot log still prints a
     * phase, and invites a confident wrong conclusion about where the firmware wedged. */
    if (!failed_before && s_wdt.write_failed && !failLogged) {
        failLogged = true;
        od_log_warn("[WDT] retained write failed - breadcrumb may be stale");
    }
}
