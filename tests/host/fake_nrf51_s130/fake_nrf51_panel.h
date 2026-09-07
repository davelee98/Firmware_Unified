#ifndef OD_TEST_FAKE_NRF51_PANEL_H
#define OD_TEST_FAKE_NRF51_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#define NRF_SUCCESS 0u
#define __NOP() od_nrf51_panel_test_nop()

void od_nrf51_panel_test_pin_set(unsigned pin);
void od_nrf51_panel_test_pin_clear(unsigned pin);
bool od_nrf51_panel_test_pin_read(unsigned pin);
void od_nrf51_panel_test_pin_output(unsigned pin);
bool od_nrf51_panel_test_spi(uint8_t value);
void od_nrf51_panel_test_configure(unsigned sclk, unsigned mosi, unsigned busy);
void od_nrf51_panel_test_disable(void);
void od_nrf51_panel_test_nop(void);

uint32_t sd_clock_hfclk_request(void);
uint32_t sd_clock_hfclk_is_running(uint32_t *running);
uint32_t sd_clock_hfclk_release(void);
uint32_t sd_app_evt_wait(void);

#endif
