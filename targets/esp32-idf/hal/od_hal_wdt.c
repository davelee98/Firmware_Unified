/* od_hal_wdt for ESP-IDF. Contract and division of labour: shared/hal/od_hal_wdt.h.
 *
 * The peripheral is the Task Watchdog Timer. It is the only watchdog IDF exposes through a
 * public API that a task can subscribe to and feed; the RTC and interrupt watchdogs are owned
 * by the bootloader and the scheduler respectively and have no supported application feed.
 */

#include "od_hal_wdt.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* --------------------------------------------------------------------------- reset cause --- */

uint32_t od_hal_wdt_reset_reason(void)
{
    /* esp_reset_reason() reports ONE cause, not a bitmask, so exactly one bit is ever set.
     * That is a narrowing the shared side tolerates -- it keys on OD_HAL_WDT_RESET_WATCHDOG
     * and prints the rest -- and not something to paper over by guessing extra bits.
     *
     * All three watchdogs fold into one bit. The strike counter asks "did a watchdog end the
     * last run", and which of the three fired is a diagnostic the target already logs by name.
     * A task-watchdog timeout arrives here as ESP_RST_TASK_WDT and not as ESP_RST_PANIC even
     * though it reaches the panic handler: the TWDT stamps the reason hint before aborting. */
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return OD_HAL_WDT_RESET_POWER_ON;
    case ESP_RST_EXT:       return OD_HAL_WDT_RESET_PIN;
    case ESP_RST_SW:        return OD_HAL_WDT_RESET_SOFTWARE;
    case ESP_RST_INT_WDT:   return OD_HAL_WDT_RESET_WATCHDOG;
    case ESP_RST_TASK_WDT:  return OD_HAL_WDT_RESET_WATCHDOG;
    case ESP_RST_WDT:       return OD_HAL_WDT_RESET_WATCHDOG;
    case ESP_RST_BROWNOUT:  return OD_HAL_WDT_RESET_BROWNOUT;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    case ESP_RST_CPU_LOCKUP: return OD_HAL_WDT_RESET_LOCKUP;
#endif
    /* PANIC, DEEPSLEEP, SDIO, USB, JTAG, EFUSE, PWR_GLITCH. Recognised, deliberately not
     * modelled: none is a watchdog, and the shared policy treats every non-watchdog reset
     * identically (it clears the strike counter). A deep-sleep wake lands here, which is why
     * OTHER must not be confused with UNKNOWN -- an ordinary wake is a known cause. */
    case ESP_RST_UNKNOWN:   return OD_HAL_WDT_RESET_UNKNOWN;
    default:                return OD_HAL_WDT_RESET_OTHER;
    }
}

/* ----------------------------------------------------------------------- retained storage --- */

/* RTC_NOINIT_ATTR, NOT RTC_DATA_ATTR. The bootloader reloads the initialised RTC data segment
 * from the app image on every reset except a deep-sleep wake, so RTC_DATA_ATTR would come back
 * zeroed after exactly the panic/watchdog/software resets this byte exists to survive. The
 * .rtc_noinit section is linked NOLOAD and is never written by startup code.
 *
 * The magic word is what makes "lost on power-on" true. RTC slow memory holds whatever it held
 * before, so a cold start reads plausible garbage; a 32-bit tag that must match exactly is the
 * cheap way to tell our byte from someone else's leftovers. The shared layout's own 2-bit tag
 * is a second filter, not a substitute -- it would pass on 1 garbage byte in 4. */
#define OD_WDT_RTC_MAGIC 0x4F44574Du /* 'ODWM' */

static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint8_t  s_rtc_byte;

bool od_hal_wdt_retained_read(uint8_t *out)
{
    if (out == NULL) {
        return false;
    }
    if (s_rtc_magic != OD_WDT_RTC_MAGIC) {
        /* Cold start or foreign content. Claim the words and hand back a value the shared tag
         * check will reject, so the policy layer establishes its own byte rather than reading
         * a strike count out of noise. */
        s_rtc_magic = OD_WDT_RTC_MAGIC;
        s_rtc_byte  = 0;
    }
    *out = s_rtc_byte;
    return true;
}

bool od_hal_wdt_retained_write(uint8_t value)
{
    s_rtc_magic = OD_WDT_RTC_MAGIC;
    s_rtc_byte  = value;
    return true;
}

/* -------------------------------------------------------------------------- the watchdog --- */

#if CONFIG_ESP_TASK_WDT_EN

/* Which idle tasks the TWDT watches, reproduced from the build's own Kconfig.
 *
 * esp_task_wdt_reconfigure() takes a whole config and there is no getter, so a reconfigure that
 * did not carry this forward would silently unsubscribe the idle tasks IDF subscribed at boot. */
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
#define OD_WDT_IDLE_CORE_0 (1u << 0)
#else
#define OD_WDT_IDLE_CORE_0 0u
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
#define OD_WDT_IDLE_CORE_1 (1u << 1)
#else
#define OD_WDT_IDLE_CORE_1 0u
#endif
#define OD_WDT_IDLE_CORE_MASK (OD_WDT_IDLE_CORE_0 | OD_WDT_IDLE_CORE_1)

/* Set once arm() subscribes the calling task. feed() is a no-op until then, and stays one
 * forever if arming failed -- resetting a TWDT this task is not subscribed to would return
 * ESP_ERR_NOT_FOUND on every loop pass. */
static bool s_subscribed;

enum od_hal_wdt_arm_result od_hal_wdt_arm(uint32_t timeout_s)
{
    esp_task_wdt_config_t cfg;
    esp_err_t err;
    bool inherited = false;

    if (timeout_s == 0u) {
        /* Disabled, and here that is genuinely all it means. The inherit hazard od_hal_wdt.h
         * describes is a watchdog that keeps running across a reset and resets a build that
         * never feeds it; the TWDT is software over a timer, re-created from scratch every
         * boot, and it cannot fire at a task that never subscribed. Leaving IDF's own
         * configuration untouched is therefore the correct disabled behaviour. */
        return OD_HAL_WDT_ARM_DISABLED;
    }

    cfg.timeout_ms     = timeout_s * 1000u; /* bounded to 3600 s by od_watchdog.h */
    cfg.idle_core_mask = OD_WDT_IDLE_CORE_MASK;
    cfg.trigger_panic  = true;

    /* SUBSCRIBING THE LOOP TASK IS WHAT THE REFERENCE DEFERRED. Firmware/src/watchdog_esp32.cpp
     * arms nothing and logs the gap instead: the IDF task watchdog is enabled and initialised,
     * but only an idle task is subscribed, so a wedged loop() is caught by nothing. It names the
     * fix as a one-liner and holds it back for want of the timeout analysis. That analysis is
     * OD_WDT_TIMEOUT_S: 300 s, derived from the ~240 s panel refresh no feed site can interrupt
     * (od_watchdog.h). With the number settled, the one-liner is the esp_task_wdt_add() below.
     *
     * CONFIG_ESP_TASK_WDT_INIT makes IDF start the TWDT before app_main, so the common path is
     * the reconfigure. Widening it to OD_WDT_TIMEOUT_S also widens the idle-task check that was
     * running at CONFIG_ESP_TASK_WDT_TIMEOUT_S, and that is the deliberate price: one timer
     * serves both, and the value has to clear that same 240 s bound. A 60 s idle check that
     * resets a device mid-refresh is not a stricter watchdog, it is a broken one. */
    err = esp_task_wdt_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        inherited = true;
        err = esp_task_wdt_reconfigure(&cfg);
    }
    if (err != ESP_OK) {
        return OD_HAL_WDT_ARM_ERROR;
    }

    /* Subscribe the CALLING task, which is the one that will feed. Doing it here rather than by
     * handle means the feed contract -- same context, never an ISR -- is enforced by
     * construction: esp_task_wdt_reset() only ever finds the task that armed. */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        return OD_HAL_WDT_ARM_ERROR;
    }
    s_subscribed = true;

    return inherited ? OD_HAL_WDT_ARM_INHERITED : OD_HAL_WDT_ARM_OK;
}

void od_hal_wdt_feed(void)
{
    if (s_subscribed) {
        (void)esp_task_wdt_reset();
    }
}

#else /* !CONFIG_ESP_TASK_WDT_EN */

enum od_hal_wdt_arm_result od_hal_wdt_arm(uint32_t timeout_s)
{
    (void)timeout_s;
    return OD_HAL_WDT_ARM_UNSUPPORTED;
}

void od_hal_wdt_feed(void)
{
}

#endif /* CONFIG_ESP_TASK_WDT_EN */
