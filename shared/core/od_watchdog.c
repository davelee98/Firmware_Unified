/* od_watchdog.c -- see od_watchdog.h for the design and what a wrong watchdog costs.
 *
 * Ported from the portable half of Firmware/src/watchdog_nrf.cpp. Every rule below was learned
 * on hardware there; none of it is new invention, and the comments carry the original reasoning
 * across rather than restating the mechanism.
 *
 * No allocation, no blocking, no kernel primitives, no vendor headers. C99.
 */
#include "od_watchdog.h"
#include "od_hal_wdt.h"

#include <string.h>

/* ------------------------------------------------------------------------------- phases --- */

const char *od_watchdog_phase_name(uint8_t phase)
{
    /* A table indexed by value, with an explicit bound check. The nRF original used a switch;
     * a table is equivalent here because the enum is dense 0..15 by construction, and it makes
     * the "16 values, field is full" constraint visible as an array length. */
    static const char *const names[] = {
        "IDLE",         "ACQUIRE_COLD", "ACQUIRE_WARM", "INIT_SEQ",
        "FILL",         "STREAM",       "REFRESH_WAIT", "RELEASE",
        "FORCE_OFF",    "BOOT_REFRESH", "IDLE_OFF",     "IDLE_WARM",
        "PWRMGM_PMIC",  "PWRMGM_RAIL",  "PWRMGM_PINS",  "PWRMGM_BUS"
    };
    if (phase >= (uint8_t)(sizeof names / sizeof names[0])) {
        return "UNKNOWN";
    }
    return names[phase];
}

/* ----------------------------------------------------------------- retained byte accessors --- */

bool od_watchdog_retained_valid(uint8_t byte)
{
    return (byte & OD_WDT_RETAINED_TAG_MASK) == OD_WDT_RETAINED_TAG_VALUE;
}

uint8_t od_watchdog_retained_strikes(uint8_t byte)
{
    return (uint8_t)((byte & OD_WDT_RETAINED_CNT_MASK) >> OD_WDT_RETAINED_CNT_SHIFT);
}

uint8_t od_watchdog_retained_phase(uint8_t byte)
{
    return (uint8_t)(byte & OD_WDT_RETAINED_PHASE_MASK);
}

uint8_t od_watchdog_retained_pack(uint8_t strikes, uint8_t phase)
{
    /* Saturate rather than truncate. A count of 4 truncated to 2 bits becomes 0, which would
     * silently reset the strike counter at the exact moment it should trip safe mode. */
    if (strikes > 3u) {
        strikes = 3u;
    }
    return (uint8_t)(OD_WDT_RETAINED_TAG_VALUE
                     | ((uint8_t)(strikes << OD_WDT_RETAINED_CNT_SHIFT) & OD_WDT_RETAINED_CNT_MASK)
                     | (phase & OD_WDT_RETAINED_PHASE_MASK));
}

/* Read-modify-write of one field, preserving the others.
 *
 * A byte that fails the tag check is REINITIALISED to a bare tag rather than merged into: it is
 * cold-boot garbage or another writer's value, and merging would carry a foreign strike count
 * or phase forward as if it were ours.
 */
static bool retained_update(struct od_watchdog *s, uint8_t mask, uint8_t value)
{
    uint8_t cur = 0;

    if (!s->retained_usable) {
        return false;
    }
    if (!od_hal_wdt_retained_read(&cur)) {
        return false;
    }
    if (!od_watchdog_retained_valid(cur)) {
        cur = OD_WDT_RETAINED_TAG_VALUE;
    }
    return od_hal_wdt_retained_write((uint8_t)((cur & (uint8_t)~mask) | (value & mask)));
}

static void strikes_store(struct od_watchdog *s, uint8_t n)
{
    if (n > 3u) {
        n = 3u;
    }
    (void)retained_update(s, OD_WDT_RETAINED_CNT_MASK,
                          (uint8_t)(n << OD_WDT_RETAINED_CNT_SHIFT));
}

/* ---------------------------------------------------------------------------- lifecycle --- */

void od_watchdog_boot_init(struct od_watchdog *s, uint32_t now_ms,
                           struct od_watchdog_boot_report *report)
{
    uint8_t  retained = 0;
    bool     readable;
    bool     valid = false;
    uint8_t  strikes = 0;
    uint32_t reason;

    if (!s) {
        return;
    }
    memset(s, 0, sizeof *s);
    s->boot_ms    = now_ms;
    s->last_phase = 0xFFu; /* nothing stamped yet; any first phase must be written */

    reason   = od_hal_wdt_reset_reason();
    readable = od_hal_wdt_retained_read(&retained);

    /* Provisionally usable so retained_update() may attempt a write; a failed write clears it
     * below. Ordering matters: without this the first write would be refused by its own guard. */
    s->retained_usable = readable;

    if (readable) {
        valid = od_watchdog_retained_valid(retained);
        if (valid) {
            strikes = od_watchdog_retained_strikes(retained);
        } else {
            /* Cold start, first boot, or a foreign value. Establish the tag so subsequent
             * breadcrumb writes have a byte of ours to modify. */
            if (!od_hal_wdt_retained_write(OD_WDT_RETAINED_TAG_VALUE)) {
                s->retained_usable = false;
            }
        }
    }

    /* --- strike accounting -------------------------------------------------------------- */
    if ((reason & OD_HAL_WDT_RESET_WATCHDOG) != 0u) {
        if (strikes < 3u) {
            strikes++;
        }
        strikes_store(s, strikes);
    } else if (strikes != 0u) {
        /* ANY non-watchdog reset means the fast-reset cycle was broken by something else -- a
         * power cycle, a pin reset, a deliberate reboot. Start clean rather than letting an old
         * count push an otherwise healthy device into safe mode. */
        strikes = 0u;
        strikes_store(s, 0u);
    }

    s->strikes         = strikes;
    s->safe_mode       = (strikes >= OD_WDT_SAFE_MODE_STRIKES);
    s->strikes_to_clear = (strikes != 0u);

    /* Stamp IDLE explicitly, which also synchronises last_phase with the register so the
     * write cache never starts out lying about what the byte contains. */
    od_watchdog_breadcrumb(s, (uint8_t)OD_WDT_PHASE_IDLE);

    if (report) {
        memset(report, 0, sizeof *report);
        report->reset_reason      = reason;
        report->was_watchdog      = (reason & OD_HAL_WDT_RESET_WATCHDOG) != 0u;
        report->retained_readable = readable;
        report->retained_writable = s->retained_usable;
        report->breadcrumb_valid  = valid;
        report->prev_phase        = valid ? od_watchdog_retained_phase(retained) : 0u;
        report->strikes           = strikes;
        report->safe_mode         = s->safe_mode;
    }
}

bool od_watchdog_in_safe_mode(const struct od_watchdog *s)
{
    return s && s->safe_mode;
}

enum od_hal_wdt_arm_result od_watchdog_arm(struct od_watchdog *s)
{
    enum od_hal_wdt_arm_result rc;

    if (!s) {
        return OD_HAL_WDT_ARM_ERROR;
    }
    rc = od_hal_wdt_arm((uint32_t)OD_WDT_TIMEOUT_S);

    /* INHERITED counts as armed. Something is running and will reset this device whether or not
     * this build started it, so every feed site must behave as if we own it. Treating inherit
     * as "not armed" is how a build with the watchdog disabled bricks itself after a DFU from
     * a build that had it enabled -- see od_hal_wdt.h. */
    s->armed = (rc == OD_HAL_WDT_ARM_OK || rc == OD_HAL_WDT_ARM_INHERITED);
    return rc;
}

/* Clear the strike counter once the device has demonstrably survived, so safe mode is
 * self-exiting and a slow-recurring fault never accumulates toward it. */
static void strikes_clear_if_healthy(struct od_watchdog *s, uint32_t now_ms)
{
    if (!s->strikes_to_clear) {
        return;
    }
    /* Unsigned subtraction, so a millisecond counter that wraps is a non-event: the difference
     * is still the true elapsed span for any interval shorter than the counter's period. */
    if ((uint32_t)(now_ms - s->boot_ms) < (uint32_t)OD_WDT_HEALTHY_MS) {
        return;
    }
    s->strikes_to_clear = false;
    s->strikes          = 0u;
    strikes_store(s, 0u);
}

void od_watchdog_feed(struct od_watchdog *s, uint32_t now_ms)
{
    if (!s) {
        return;
    }
    strikes_clear_if_healthy(s, now_ms);

    /* Feed unconditionally rather than gating on s->armed. The HAL already no-ops when nothing
     * is armed, and it is the only layer that can see an inherited watchdog this module never
     * armed -- a gate here would suppress exactly the feed that prevents the brick. */
    od_hal_wdt_feed();
}

void od_watchdog_breadcrumb(struct od_watchdog *s, uint8_t phase)
{
    if (!s) {
        return;
    }
    phase = (uint8_t)(phase & OD_WDT_RETAINED_PHASE_MASK);
    if (phase == s->last_phase) {
        return;
    }
    /* Advance the cache ONLY on success. Updating it first would suppress the retry after a
     * failed write, leaving the cache claiming a phase the retained byte never received -- and
     * on a target whose write is a clear+set pair, a failure can even leave the byte cleared. */
    if (!retained_update(s, OD_WDT_RETAINED_PHASE_MASK, phase)) {
        s->write_failed = true; /* latched; this sits on per-frame paths */
        return;
    }
    s->last_phase = phase;
}
