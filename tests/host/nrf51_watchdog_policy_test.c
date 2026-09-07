#include "fake_nrf51_main.h"
#include "od_nrf51_profile.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void RTC1_IRQHandler(void);
uint32_t od_nrf51_now_ms(void);
void od_nrf51_main_test_reset(void);
bool od_nrf51_main_test_heartbeat_start(void);
void od_nrf51_main_test_watchdog_start(void);
void od_nrf51_main_test_watchdog_feed(void);
void od_nrf51_main_test_runtime_progress(void);

od_test_rtc_t od_test_rtc1;
od_test_wdt_t od_test_wdt;

static int failures;
static uint32_t priority_result;
static uint32_t enable_result;
static unsigned priority_calls;
static unsigned enable_calls;
static bool ble_healthy;
static unsigned progress_calls;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

uint32_t sd_nvic_SetPriority(int irq, uint32_t priority)
{
    CHECK(irq == RTC1_IRQn && priority == 3u);
    priority_calls++;
    return priority_result;
}

uint32_t sd_nvic_EnableIRQ(int irq)
{
    CHECK(irq == RTC1_IRQn);
    enable_calls++;
    return enable_result;
}

bool od_nrf51_ble_healthy(void)
{
    return ble_healthy;
}

void od_nrf51_ble_note_progress(void)
{
    progress_calls++;
}

static void reset_fakes(void)
{
    memset(&od_test_rtc1, 0, sizeof(od_test_rtc1));
    memset(&od_test_wdt, 0, sizeof(od_test_wdt));
    priority_result = NRF_SUCCESS;
    enable_result = NRF_SUCCESS;
    priority_calls = 0u;
    enable_calls = 0u;
    ble_healthy = true;
    progress_calls = 0u;
    od_nrf51_main_test_reset();
}

static void test_watchdog(void)
{
    const uint32_t expected_reload = OD_NRF51_WATCHDOG_TIMEOUT_MS * 32768u / 1000u;

    reset_fakes();
    od_nrf51_main_test_watchdog_feed();
    CHECK(od_test_wdt.RR[0] == 0u);
    od_nrf51_main_test_watchdog_start();
    CHECK(od_test_wdt.CONFIG ==
          ((WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
           (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos)));
    CHECK(od_test_wdt.CRV == expected_reload);
    CHECK(od_test_wdt.RREN == (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos));
    CHECK(od_test_wdt.TASKS_START == 1u);
    od_nrf51_main_test_watchdog_feed();
    CHECK(od_test_wdt.RR[0] == WDT_RR_RR_Reload);

    od_test_wdt.CRV = 123u;
    od_nrf51_main_test_watchdog_start();
    CHECK(od_test_wdt.CRV == 123u);

    od_test_wdt.RR[0] = 0u;
    ble_healthy = false;
    od_nrf51_main_test_runtime_progress();
    CHECK(progress_calls == 0u && od_test_wdt.RR[0] == 0u);
    ble_healthy = true;
    od_nrf51_main_test_runtime_progress();
    CHECK(progress_calls == 1u && od_test_wdt.RR[0] == WDT_RR_RR_Reload);
}

static void test_heartbeat(void)
{
    const uint32_t ticks = OD_NRF51_HEARTBEAT_MS * 32768u / 1000u;

    reset_fakes();
    od_test_rtc1.COUNTER = 123u;
    CHECK(od_nrf51_main_test_heartbeat_start());
    CHECK(priority_calls == 1u && enable_calls == 1u);
    CHECK(od_test_rtc1.CC[0] == ((123u + ticks) & 0xFFFFFFu));
    CHECK(od_test_rtc1.INTENSET ==
          (RTC_INTENSET_OVRFLW_Msk | RTC_INTENSET_COMPARE0_Msk));
    CHECK(od_test_rtc1.TASKS_START == 1u);

    od_test_wdt.RR[0] = 0u;
    od_test_rtc1.COUNTER = 456u;
    od_test_rtc1.EVENTS_COMPARE[0] = 1u;
    RTC1_IRQHandler();
    CHECK(od_test_rtc1.EVENTS_COMPARE[0] == 0u);
    CHECK(od_test_rtc1.CC[0] == ((456u + ticks) & 0xFFFFFFu));
    CHECK(od_test_wdt.RR[0] == 0u);

    reset_fakes();
    priority_result = 1u;
    CHECK(!od_nrf51_main_test_heartbeat_start());
    CHECK(priority_calls == 1u && enable_calls == 0u);
    reset_fakes();
    enable_result = 1u;
    CHECK(!od_nrf51_main_test_heartbeat_start());
    CHECK(priority_calls == 1u && enable_calls == 1u);
}

static void test_clock(void)
{
    reset_fakes();
    od_test_rtc1.COUNTER = 32768u;
    CHECK(od_nrf51_now_ms() == 1000u);
    od_test_rtc1.EVENTS_OVRFLW = 1u;
    od_test_rtc1.COUNTER = 1u;
    CHECK(od_nrf51_now_ms() == 512000u);
    RTC1_IRQHandler();
    CHECK(od_nrf51_now_ms() == 512000u);
}

int main(void)
{
    test_watchdog();
    test_heartbeat();
    test_clock();
    return failures == 0 ? 0 : 1;
}
