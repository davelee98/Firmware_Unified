#ifndef OD_TEST_FAKE_NRF51_MAIN_H
#define OD_TEST_FAKE_NRF51_MAIN_H

#include <stdint.h>

#define NRF_SUCCESS 0u
#define RTC1_IRQn 17

#define RTC_INTENSET_OVRFLW_Msk 0x02u
#define RTC_INTENSET_COMPARE0_Msk 0x10000u

#define WDT_CONFIG_SLEEP_Pos 0u
#define WDT_CONFIG_SLEEP_Run 1u
#define WDT_CONFIG_HALT_Pos 3u
#define WDT_CONFIG_HALT_Pause 0u
#define WDT_RREN_RR0_Pos 0u
#define WDT_RREN_RR0_Enabled 1u
#define WDT_RR_RR_Reload 0x6E524635u

typedef struct {
    uint32_t TASKS_START;
    uint32_t COUNTER;
    uint32_t PRESCALER;
    uint32_t EVENTS_OVRFLW;
    uint32_t EVENTS_COMPARE[4];
    uint32_t INTENSET;
    uint32_t CC[4];
} od_test_rtc_t;

typedef struct {
    uint32_t TASKS_START;
    uint32_t CONFIG;
    uint32_t CRV;
    uint32_t RREN;
    uint32_t RR[8];
} od_test_wdt_t;

typedef struct {
    uint32_t reserved;
} nrf_nvic_state_t;

extern od_test_rtc_t od_test_rtc1;
extern od_test_wdt_t od_test_wdt;

#define NRF_RTC1 (&od_test_rtc1)
#define NRF_WDT (&od_test_wdt)

uint32_t sd_nvic_SetPriority(int irq, uint32_t priority);
uint32_t sd_nvic_EnableIRQ(int irq);

#endif
