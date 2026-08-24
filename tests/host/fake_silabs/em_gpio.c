/* GPIO and delay stand-ins that RECORD EDGES, for the BG22 I2C transport suite. */

#include "em_gpio.h"
#include "sl_udelay.h"

#include <string.h>

enum gpio_edge gpio_trace[GPIO_TRACE_MAX];
unsigned       gpio_trace_len;
unsigned long  fake_udelay_total_us;

static GPIO_Port_TypeDef s_scl_port, s_sda_port;
static uint8_t           s_scl_pin,  s_sda_pin;

static uint8_t  s_sda_bits[256];
static unsigned s_sda_bit_count;
static unsigned s_sda_bit_pos;
static bool     s_sda_is_input;

static void emit(enum gpio_edge e)
{
    if (gpio_trace_len < GPIO_TRACE_MAX) {
        gpio_trace[gpio_trace_len] = e;
    }
    gpio_trace_len++;
}

static bool is_scl(GPIO_Port_TypeDef p, uint8_t n) { return p == s_scl_port && n == s_scl_pin; }
static bool is_sda(GPIO_Port_TypeDef p, uint8_t n) { return p == s_sda_port && n == s_sda_pin; }

void fake_gpio_reset(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                     GPIO_Port_TypeDef sda_port, uint8_t sda_pin)
{
    memset(gpio_trace, 0, sizeof gpio_trace);
    gpio_trace_len = 0;
    fake_udelay_total_us = 0;
    s_scl_port = scl_port; s_scl_pin = scl_pin;
    s_sda_port = sda_port; s_sda_pin = sda_pin;
    s_sda_bit_count = 0;
    s_sda_bit_pos = 0;
    s_sda_is_input = false;
}

void fake_gpio_set_sda_reads(const uint8_t *bits, unsigned count)
{
    if (count > sizeof s_sda_bits) {
        count = (unsigned)sizeof s_sda_bits;
    }
    memcpy(s_sda_bits, bits, count);
    s_sda_bit_count = count;
    s_sda_bit_pos = 0;
}

void GPIO_PinModeSet(GPIO_Port_TypeDef port, uint8_t pin, GPIO_Mode_TypeDef mode, unsigned out)
{
    (void)out;
    if (is_sda(port, pin)) {
        s_sda_is_input = (mode == gpioModeInputPull);
        emit(s_sda_is_input ? GPIO_EDGE_SDA_INPUT : GPIO_EDGE_SDA_OUTPUT);
    }
}

void GPIO_PinOutSet(GPIO_Port_TypeDef port, uint8_t pin)
{
    if (is_scl(port, pin)) { emit(GPIO_EDGE_SCL_HIGH); }
    else if (is_sda(port, pin)) { emit(GPIO_EDGE_SDA_HIGH); }
}

void GPIO_PinOutClear(GPIO_Port_TypeDef port, uint8_t pin)
{
    if (is_scl(port, pin)) { emit(GPIO_EDGE_SCL_LOW); }
    else if (is_sda(port, pin)) { emit(GPIO_EDGE_SDA_LOW); }
}

unsigned GPIO_PinInGet(GPIO_Port_TypeDef port, uint8_t pin)
{
    if (!is_sda(port, pin)) {
        return 1u;
    }
    if (s_sda_bit_pos < s_sda_bit_count) {
        return s_sda_bits[s_sda_bit_pos++] ? 1u : 0u;
    }
    return 1u;   /* nothing scripted: the line floats high, i.e. NACK / all-ones data */
}

void sl_udelay_wait(uint32_t us) { fake_udelay_total_us += us; }
