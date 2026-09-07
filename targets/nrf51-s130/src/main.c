#include "od_nrf51_ble.h"
#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"

#include <nrf.h>
#include <nrf_nvic.h>
#include <nrf_soc.h>

#include <stdbool.h>
#include <stdint.h>

#define RTC_TICKS_PER_HEARTBEAT ((OD_NRF51_HEARTBEAT_MS * 32768u) / 1000u)
#define WDT_RELOAD_TICKS ((OD_NRF51_WATCHDOG_TIMEOUT_MS * 32768u) / 1000u)

nrf_nvic_state_t nrf_nvic_state = { 0 };

static volatile uint32_t rtc_epochs;
static bool watchdog_started;

static void heartbeat_arm(void)
{
    NRF_RTC1->CC[0] = (NRF_RTC1->COUNTER + RTC_TICKS_PER_HEARTBEAT) & 0xFFFFFFu;
}

void RTC1_IRQHandler(void)
{
    if (NRF_RTC1->EVENTS_OVRFLW != 0u) {
        NRF_RTC1->EVENTS_OVRFLW = 0u;
        rtc_epochs++;
    }
    if (NRF_RTC1->EVENTS_COMPARE[0] != 0u) {
        NRF_RTC1->EVENTS_COMPARE[0] = 0u;
        heartbeat_arm();
    }
}

uint32_t od_nrf51_now_ms(void)
{
    uint32_t epochs;
    uint32_t counter;

    do {
        epochs = rtc_epochs;
        counter = NRF_RTC1->COUNTER;
    } while (epochs != rtc_epochs);
    if (NRF_RTC1->EVENTS_OVRFLW != 0u && counter < 0x800000u) {
        epochs++;
    }
    return epochs * 512000u + counter * 125u / 4096u;
}

static bool heartbeat_start(void)
{
    NRF_RTC1->PRESCALER = 0u;
    NRF_RTC1->EVENTS_OVRFLW = 0u;
    NRF_RTC1->EVENTS_COMPARE[0] = 0u;
    heartbeat_arm();
    NRF_RTC1->INTENSET = RTC_INTENSET_OVRFLW_Msk | RTC_INTENSET_COMPARE0_Msk;
    if (sd_nvic_SetPriority(RTC1_IRQn, 3u) != NRF_SUCCESS ||
        sd_nvic_EnableIRQ(RTC1_IRQn) != NRF_SUCCESS) {
        return false;
    }
    NRF_RTC1->TASKS_START = 1u;
    return true;
}

static void watchdog_start(void)
{
    NRF_WDT->CONFIG =
        (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
        (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
    NRF_WDT->CRV = WDT_RELOAD_TICKS;
    NRF_WDT->RREN = WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos;
    NRF_WDT->TASKS_START = 1u;
    watchdog_started = true;
}

static void watchdog_feed(void)
{
    if (watchdog_started) {
        NRF_WDT->RR[0] = WDT_RR_RR_Reload;
    }
}

int main(void)
{
    od_nrf51_slim_init();
    watchdog_start();
    od_nrf51_ble_init();
    if (!od_nrf51_ble_healthy() || !heartbeat_start()) {
        for (;;) {
            __WFE();
        }
    }
    for (;;) {
        const bool had_event = od_nrf51_ble_process_one();

        od_nrf51_slim_tick();
        od_nrf51_ble_flush();
        if (od_nrf51_ble_healthy()) {
            od_nrf51_ble_note_progress();
            watchdog_feed();
        }
        if (!had_event) {
            (void)sd_app_evt_wait();
        }
    }
}
