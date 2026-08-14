/* od_watchdog.h -- portable watchdog policy: strike counting, safe mode, breadcrumbs.
 *
 * WHO IMPLEMENTS od_hal_wdt.h. targets/esp32-idf (hal/od_hal_wdt.c, over the Task Watchdog) and
 * targets/nordic-zephyr (src/od_hal_wdt.c, over the devicetree watchdog0 and gpregret2 nodes).
 * targets/efr32bg22-slc does not, and therefore does not compile this file.
 *
 * THE PROBLEM THIS SOLVES. targets/nordic-zephyr had no watchdog at all, while the Arduino
 * nRF52840 it replaces ships one (Firmware/src/watchdog_nrf.cpp). This is the portable half of
 * closing that gap, and the half that must behave identically on every chip.
 *
 * WHAT A WATCHDOG COSTS IF YOU GET IT WRONG, stated first because it drives every decision
 * below: a watchdog that resets a device which wedges during BOOT turns one hang into an
 * endless reset cycle. The device never advertises, is unreachable over BLE and DFU, and
 * flattens its battery faster than if it had simply hung. That is strictly worse than the bug
 * it was added to fix. The strike counter and safe mode exist to escape exactly that, and they
 * are not optional extras.
 *
 * DIVISION OF LABOUR. shared/ owns policy; targets/ owns registers (od_hal_wdt.h). The split
 * follows the real one in Firmware/src/watchdog_nrf.cpp, roughly a third of which is portable
 * logic that had nowhere to live: the retained-byte layout, the strike state machine, the
 * uptime rule, and the breadcrumb cache are all chip-independent.
 *
 * THIS MODULE DOES NOT LOG. od_log.h is target-local and shared/ may not include it, so
 * od_watchdog_boot_init() fills a struct od_watchdog_boot_report and the target logs it. The
 * nRF original logs eight distinct lines at boot; every fact behind them is in that report, and
 * a target that does not log it loses the diagnostic, not the behaviour.
 *
 * CONCURRENCY. Single-owner, like od_adv_control: every function is called from ONE context and
 * it is the caller's job to keep it that way. No locks, no atomics, no allocation, no blocking.
 */
#ifndef OD_WATCHDOG_H
#define OD_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* For enum od_hal_wdt_arm_result, which od_watchdog_arm() returns unchanged. Including the HAL
 * from a core header is the exception rather than the habit -- od_adv_control.h deliberately
 * does not -- and it is here only because passing the arm outcome through verbatim is more
 * honest than re-encoding four values into a parallel enum that could drift. */
#include "od_hal_wdt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------- build config --- */

/* Watchdog period in seconds. 0 disables arming (the inherit path in od_hal_wdt_arm still
 * applies -- see that header for why disabling must not mean ignoring a running watchdog).
 *
 * Plain preprocessor constant, not Kconfig: Silabs has no Kconfig, and CLAUDE.md decision 7
 * makes preprocessor constants the shared config surface. Kconfig is only how two of the three
 * targets set it.
 *
 * THE FLOOR IS A REAL MEASUREMENT, NOT A ROUND NUMBER. The timeout must exceed every legitimate
 * blocking span in the firmware. The largest known is a ~240 s bbepRefresh() on a 7-colour
 * split-buffer panel (4 x 30 s BUSY_WAIT x 2 controllers), and the code cannot feed from inside
 * that third-party call. The shipping Arduino build sets 120 s, which is BELOW that span --
 * recorded as finding H-3 in Firmware/docs/CODE_REVIEW_2026-08-04.md, i.e. a known way to reset
 * a device that is working correctly. 300 s is the plan's original derivation and the default
 * here; do not lower it below 300 without re-deriving the panel bound first.
 */
#ifndef OD_WDT_TIMEOUT_S
#define OD_WDT_TIMEOUT_S 300u
#endif

#if OD_WDT_TIMEOUT_S != 0 && (OD_WDT_TIMEOUT_S < 60 || OD_WDT_TIMEOUT_S > 3600)
/* Upper bound rejects a mistyped value that would silently disable recovery for hours; the
 * representable maximum is far higher (nRF CRV is 32 bits at 32768 Hz, ~131072 s). */
#error "OD_WDT_TIMEOUT_S must be 0 (disabled) or in 60..3600 seconds"
#endif

/* Consecutive watchdog resets before this boot skips all panel work. */
#define OD_WDT_SAFE_MODE_STRIKES 3u

/* Continuous uptime that proves the device is not boot-looping, clearing the strike counter.
 *
 * AN UPTIME RULE, NOT A "FIRST SUCCESSFUL REFRESH" RULE, and the alternative fails twice over:
 * every ordinary boot performs a successful boot refresh, so strikes could never accumulate;
 * and safe mode performs NO refresh, so it could never satisfy the clear condition and would be
 * permanent. Uptime is panel-independent and therefore behaves identically inside safe mode.
 * Must be at least twice the timeout so one slow-but-healthy period cannot clear a real strike.
 */
#define OD_WDT_HEALTHY_MS (10UL * 60UL * 1000UL)

/* --------------------------------------------------------------------- breadcrumb phases --- */

/* Where the firmware was when it stopped making progress. Retained across a reset, so the boot
 * AFTER a watchdog reset can name the wait that wedged instead of reporting only that something
 * did.
 *
 * MUST FIT 4 BITS (0..15) -- see the retained-byte layout below. The list is full at 16 values;
 * adding a phase means retiring one, not widening the field, because the field cannot widen
 * without breaking the one-byte contract in od_hal_wdt.h.
 *
 * Values are carried over unchanged from Firmware/src/watchdog.h so a device flashed across the
 * two firmwares decodes its own retained byte consistently.
 */
enum od_wdt_phase {
    OD_WDT_PHASE_IDLE           = 0,  /* pre-session; stamped once at boot */
    OD_WDT_PHASE_ACQUIRE_COLD   = 1,
    OD_WDT_PHASE_ACQUIRE_WARM   = 2,
    OD_WDT_PHASE_INIT_SEQ       = 3,
    OD_WDT_PHASE_FILL           = 4,
    OD_WDT_PHASE_STREAM         = 5,
    OD_WDT_PHASE_REFRESH_WAIT   = 6,
    OD_WDT_PHASE_RELEASE        = 7,
    OD_WDT_PHASE_FORCE_OFF      = 8,
    OD_WDT_PHASE_BOOT_REFRESH   = 9,
    /* Idle sub-states. Power-management state is plain RAM and does not survive a reset, so
     * without a distinct phase per sub-state a freeze in either reports the same generic IDLE
     * and the two are indistinguishable after the fact. */
    OD_WDT_PHASE_IDLE_OFF       = 10, /* session fully powered down */
    OD_WDT_PHASE_IDLE_WARM      = 11, /* panel kept awake for keep-alive */
    /* Rail bring-up sub-steps. Uninstrumented until a 2026-08-03 freeze that reset ~120 s after
     * ACQUIRE_COLD without ever reaching INIT_SEQ -- the wedge was inside bring-up itself, and
     * the breadcrumb could not say where. These four names close that blind spot. */
    OD_WDT_PHASE_PWRMGM_PMIC    = 12, /* before PMIC bring-up over I2C */
    OD_WDT_PHASE_PWRMGM_RAIL    = 13, /* before panel rail enable + settle */
    OD_WDT_PHASE_PWRMGM_PINS    = 14, /* before panel GPIO setup + settle */
    OD_WDT_PHASE_PWRMGM_BUS     = 15, /* before panel bus init */
    OD_WDT_PHASE__MAX           = 15
};

/* Stable name for a phase value, for the target's boot log. Never NULL; an out-of-range value
 * returns "UNKNOWN" rather than indexing off the end. */
const char *od_watchdog_phase_name(uint8_t phase);

/* ------------------------------------------------------------------- retained byte layout --- */

/* The single retained byte (od_hal_wdt.h) carries three fields:
 *
 *     bit  7 6 | 5 4 | 3 2 1 0
 *          tag | cnt | phase
 *
 *   7:6  validity tag, always 0b10. Distinguishes a value we wrote from cold-boot garbage or
 *        another writer. A bad tag means DISCARD -- not "assume zero", which would silently
 *        report phase IDLE and strikes 0 for a byte that was never ours.
 *   5:4  consecutive watchdog-reset strikes, 0..3, saturating.
 *   3:0  breadcrumb phase (enum od_wdt_phase).
 *
 * The allocation is explicit because without it a breadcrumb write -- which happens on
 * per-frame paths -- would destroy the strike counter, and boot-loop containment would fail
 * exactly when it is needed.
 */
#define OD_WDT_RETAINED_TAG_MASK   0xC0u
#define OD_WDT_RETAINED_TAG_VALUE  0x80u /* 0b10 << 6 */
#define OD_WDT_RETAINED_CNT_MASK   0x30u
#define OD_WDT_RETAINED_CNT_SHIFT  4u
#define OD_WDT_RETAINED_PHASE_MASK 0x0Fu

/* Pure accessors over that layout. Exposed rather than kept static so the host tests can assert
 * the encoding directly -- a layout bug is otherwise only observable through a reset. */
bool    od_watchdog_retained_valid(uint8_t byte);
uint8_t od_watchdog_retained_strikes(uint8_t byte);
uint8_t od_watchdog_retained_phase(uint8_t byte);
uint8_t od_watchdog_retained_pack(uint8_t strikes, uint8_t phase);

/* -------------------------------------------------------------------------------- state --- */

/* What boot_init() learned. The target logs this; shared/ cannot (see the file header). */
struct od_watchdog_boot_report {
    uint32_t reset_reason;      /* raw OD_HAL_WDT_RESET_* bitmask, for the log */
    bool     was_watchdog;      /* previous boot ended in a watchdog reset */
    bool     retained_readable; /* the retained store answered a read */
    bool     retained_writable; /* ...and accepted a write */
    bool     breadcrumb_valid;  /* a tagged byte survived; prev_phase is meaningful */
    uint8_t  prev_phase;        /* where the previous run last reported being */
    uint8_t  strikes;           /* AFTER this boot's accounting */
    bool     safe_mode;         /* this boot must skip all panel work */
};

struct od_watchdog {
    bool     armed;              /* a hardware watchdog is armed or inherited */
    bool     safe_mode;
    bool     strikes_to_clear;   /* a nonzero count is waiting on the uptime rule */
    bool     retained_usable;    /* reads and writes both worked at boot */
    uint32_t boot_ms;            /* timestamp passed to boot_init */
    uint8_t  strikes;
    uint8_t  last_phase;         /* breadcrumb write cache; 0xFF = unknown */
    bool     write_failed;       /* latched: a retained write failed at least once */
};

/* ------------------------------------------------------------------------------- lifecycle --- */

/* Decode the boot, evaluate the strike counter, and stamp the IDLE breadcrumb.
 *
 * Call ONCE, early, BEFORE od_watchdog_arm(). now_ms is the caller's monotonic millisecond
 * clock; it is passed in rather than taken from a time HAL so the uptime rule is directly
 * testable and shared/ needs no clock interface of its own.
 *
 * report may be NULL if the target does not log, though it usually should not be.
 */
void od_watchdog_boot_init(struct od_watchdog *s, uint32_t now_ms,
                           struct od_watchdog_boot_report *report);

/* True when the strike counter tripped and this boot must skip ALL panel work so the device
 * stays reachable over BLE and DFU.
 *
 * CALLERS MUST BRANCH ON THIS, not merely consult it. Firmware/docs/CODE_REVIEW_2026-08-04.md
 * finding H-2 is precisely this failure: panel-session refusal and successful acquisition
 * shared one boolean return, so callers continued either way and safe mode did not actually
 * prevent panel work. A safe mode that is checked but not obeyed is not a safe mode.
 */
bool od_watchdog_in_safe_mode(const struct od_watchdog *s);

/* Arm the hardware watchdog via the HAL. Call ONCE, after boot_init, before the boot panel
 * path. Returns the HAL result so the target can log which of arm/inherit/disabled happened. */
enum od_hal_wdt_arm_result od_watchdog_arm(struct od_watchdog *s);

/* Prove forward progress, and run the uptime rule that clears strikes.
 *
 * Cheap enough for a tight poll loop: the strike check early-outs on a bool.
 *
 * FEED ONLY FROM SITES WHOSE EXECUTION PROVES THE PROGRAM IS ALIVE -- the loop, cooperative
 * waits that return, and immediately before entering a bounded library call. NEVER from an ISR,
 * timer or stack callback. An interrupt-fed watchdog verifies that the interrupt controller is
 * running, not that the program is, which is the classic way to build a watchdog that never
 * fires.
 */
void od_watchdog_feed(struct od_watchdog *s, uint32_t now_ms);

/* Record the phase about to be entered. Retained across a reset.
 *
 * Writes are skipped when the phase has not changed. That is a real optimisation, not a
 * micro-one: on nRF a stamp costs up to three SoftDevice SVCs once the stack is enabled, and
 * these sit on per-frame and per-row paths where an unconditional write would be too expensive
 * to place where it is most useful.
 */
void od_watchdog_breadcrumb(struct od_watchdog *s, uint8_t phase);

#ifdef __cplusplus
}
#endif

#endif /* OD_WATCHDOG_H */
