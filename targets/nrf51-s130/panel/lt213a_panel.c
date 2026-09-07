// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * LT213A monochrome OTP/full-refresh driver.
 * Derived from greentags-lt213a-opendisplay commit
 * 28df8b6ce1506b82665809763dc92180b616e916 (GPL-3.0-or-later).
 */
#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"

#ifdef OD_NRF51_PANEL_HOST_TEST
#include "fake_nrf51_panel.h"
#else
#include <nrf.h>
#include <nrf_soc.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PIN_SCLK = 0,
    PIN_CS = 1,
    PIN_DC = 2,
    PIN_RESET = 3,
    PIN_BUSY = 4,
    PIN_BS = 5,
    PIN_MOSI = 30
};

static bool powered;
static bool refreshing;
static bool powering_off;
static bool hfclk_held;
static uint16_t received;
static uint32_t phase_started_ms;
static uint32_t refresh_started_ms;

static void pin_set(unsigned pin)
{
#ifdef OD_NRF51_PANEL_HOST_TEST
    od_nrf51_panel_test_pin_set(pin);
#else
    NRF_GPIO->OUTSET = 1u << pin;
#endif
}

static void pin_clear(unsigned pin)
{
#ifdef OD_NRF51_PANEL_HOST_TEST
    od_nrf51_panel_test_pin_clear(pin);
#else
    NRF_GPIO->OUTCLR = 1u << pin;
#endif
}

static bool pin_read(unsigned pin)
{
#ifdef OD_NRF51_PANEL_HOST_TEST
    return od_nrf51_panel_test_pin_read(pin);
#else
    return ((NRF_GPIO->IN >> pin) & 1u) != 0u;
#endif
}

static void pin_output(unsigned pin)
{
#ifdef OD_NRF51_PANEL_HOST_TEST
    od_nrf51_panel_test_pin_output(pin);
#else
    NRF_GPIO->PIN_CNF[pin] =
        (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
        (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
#endif
}

static void delay_ms(uint32_t duration)
{
    const uint32_t start = od_nrf51_now_ms();
    while ((uint32_t)(od_nrf51_now_ms() - start) < duration) {
        __NOP();
    }
}

static bool spi_byte(uint8_t value)
{
    if (!powered) {
        return false;
    }
#ifdef OD_NRF51_PANEL_HOST_TEST
    return od_nrf51_panel_test_spi(value);
#else
    const uint32_t start = od_nrf51_now_ms();

    NRF_SPI0->EVENTS_READY = 0u;
    NRF_SPI0->TXD = value;
    while (NRF_SPI0->EVENTS_READY == 0u) {
        if ((uint32_t)(od_nrf51_now_ms() - start) >= 20u) {
            od_nrf51_panel_abort();
            return false;
        }
    }
    (void)NRF_SPI0->RXD;
    return true;
#endif
}

static bool command(uint8_t value)
{
    pin_clear(PIN_DC);
    pin_clear(PIN_CS);
    if (!spi_byte(value)) {
        return false;
    }
    pin_set(PIN_CS);
    pin_set(PIN_DC);
    return true;
}

static bool data(const uint8_t *values, uint16_t length)
{
    pin_set(PIN_DC);
    while (length-- != 0u) {
        pin_clear(PIN_CS);
        if (!spi_byte(*values++)) {
            return false;
        }
        pin_set(PIN_CS);
    }
    return true;
}

static bool reg_write(uint8_t address, const uint8_t *values, uint16_t length)
{
    return command(address) && data(values, length);
}

static bool panel_ready(uint32_t timeout_ms)
{
    const uint32_t start = od_nrf51_now_ms();

    for (;;) {
        if (!command(0x71u)) {
            return false;
        }
        if (pin_read(PIN_BUSY)) {
            return true;
        }
        if ((uint32_t)(od_nrf51_now_ms() - start) >= timeout_ms) {
            return false;
        }
        delay_ms(1u);
    }
}

static bool request_hfclk(void)
{
    const uint32_t start = od_nrf51_now_ms();
    uint32_t running = 0u;

    if (sd_clock_hfclk_request() != NRF_SUCCESS) {
        return false;
    }
    hfclk_held = true;
    do {
        if (sd_clock_hfclk_is_running(&running) != NRF_SUCCESS) {
            return false;
        }
        if (running != 0u) {
            return true;
        }
        (void)sd_app_evt_wait();
    } while ((uint32_t)(od_nrf51_now_ms() - start) < 100u);
    return false;
}

void od_nrf51_panel_abort(void)
{
    if (powered) {
        pin_clear(PIN_RESET);
        pin_clear(PIN_DC);
        pin_set(PIN_CS);
#ifdef OD_NRF51_PANEL_HOST_TEST
        od_nrf51_panel_test_disable();
#else
        NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Disabled;
#endif
    }
    powered = false;
    refreshing = false;
    powering_off = false;
    received = 0u;
    if (hfclk_held) {
        (void)sd_clock_hfclk_release();
        hfclk_held = false;
    }
}

bool od_nrf51_panel_begin(void)
{
    static const uint8_t boost[] = { 0x17u, 0x17u, 0x17u };
    static const uint8_t panel[] = { 0x13u, 0x0Du };
    static const uint8_t resolution[] = { OD_NRF51_WIDTH, 0u, OD_NRF51_HEIGHT };
    static const uint8_t interval = 0x97u;
    static const uint8_t white = 0xFFu;

    if (powered || refreshing || !request_hfclk()) {
        od_nrf51_panel_abort();
        return false;
    }
    pin_set(PIN_CS);
    pin_clear(PIN_RESET);
    pin_clear(PIN_BS);
    pin_output(PIN_CS);
    pin_output(PIN_DC);
    pin_output(PIN_RESET);
    pin_output(PIN_BS);
#ifdef OD_NRF51_PANEL_HOST_TEST
    od_nrf51_panel_test_configure(PIN_SCLK, PIN_MOSI, PIN_BUSY);
#else
    NRF_GPIO->PIN_CNF[PIN_BUSY] = GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos;
    pin_clear(PIN_SCLK);
    pin_clear(PIN_MOSI);
    NRF_GPIO->PIN_CNF[PIN_SCLK] = GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos;
    pin_output(PIN_MOSI);
    NRF_SPI0->PSELSCK = PIN_SCLK;
    NRF_SPI0->PSELMOSI = PIN_MOSI;
    NRF_SPI0->PSELMISO = 0xFFFFFFFFu;
    NRF_SPI0->FREQUENCY = SPI_FREQUENCY_FREQUENCY_M2;
    NRF_SPI0->CONFIG = 0u;
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled;
#endif
    powered = true;
    refreshing = false;
    powering_off = false;
    received = 0u;

    delay_ms(10u);
    pin_set(PIN_RESET);
    delay_ms(10u);
    if (!reg_write(0x06u, boost, sizeof(boost)) ||
        !command(0x04u) || !panel_ready(5000u) ||
        !reg_write(0x00u, panel, sizeof(panel)) ||
        !reg_write(0x61u, resolution, sizeof(resolution)) ||
        !reg_write(0x50u, &interval, 1u) ||
        !command(0x10u)) {
        od_nrf51_panel_abort();
        return false;
    }
    for (uint16_t i = 0u; i < OD_NRF51_IMAGE_BYTES; ++i) {
        if (!data(&white, 1u)) {
            od_nrf51_panel_abort();
            return false;
        }
    }
    delay_ms(2u);
    if (!command(0x13u)) {
        od_nrf51_panel_abort();
        return false;
    }
    return true;
}

bool od_nrf51_panel_write(const uint8_t *values, uint16_t length)
{
    if (!powered || refreshing || values == NULL || length == 0u ||
        length > (uint16_t)(OD_NRF51_IMAGE_BYTES - received) ||
        !data(values, length)) {
        od_nrf51_panel_abort();
        return false;
    }
    received = (uint16_t)(received + length);
    return true;
}

bool od_nrf51_panel_refresh_start(void)
{
    if (!powered || refreshing || received != OD_NRF51_IMAGE_BYTES) {
        od_nrf51_panel_abort();
        return false;
    }
    delay_ms(2u);
    if (!command(0x12u)) {
        od_nrf51_panel_abort();
        return false;
    }
    phase_started_ms = od_nrf51_now_ms();
    refresh_started_ms = phase_started_ms;
    refreshing = true;
    return true;
}

int od_nrf51_panel_poll(void)
{
    const uint32_t elapsed = od_nrf51_now_ms() - phase_started_ms;

    if (!refreshing) {
        return -1;
    }
    if ((uint32_t)(od_nrf51_now_ms() - refresh_started_ms) >=
        OD_NRF51_PANEL_TIMEOUT_MS) {
        od_nrf51_panel_abort();
        return -1;
    }
    if (elapsed < (powering_off ? 1u : 100u)) {
        return 0;
    }
    if (!command(0x71u)) {
        od_nrf51_panel_abort();
        return -1;
    }
    if (!pin_read(PIN_BUSY)) {
        return 0;
    }
    if (!powering_off) {
        static const uint8_t border_off = 0xF7u;
        if (!reg_write(0x50u, &border_off, 1u) || !command(0x02u)) {
            od_nrf51_panel_abort();
            return -1;
        }
        powering_off = true;
        phase_started_ms = od_nrf51_now_ms();
        return 0;
    }
    {
        static const uint8_t check = 0xA5u;
        if (!reg_write(0x07u, &check, 1u)) {
            od_nrf51_panel_abort();
            return -1;
        }
    }
    od_nrf51_panel_abort();
    return 1;
}
