#include "od_gpio_trace.h"

#include <string.h>

enum nrf_edge nrf_trace[NRF_TRACE_MAX];
unsigned      nrf_trace_len;
unsigned      nrf_gpio_foreign_pin_writes;

static uint8_t  s_scl_cfg, s_sda_cfg;
static uint8_t  s_bits[256];
static unsigned s_bit_count, s_bit_pos;
static bool     s_sda_released;

static void emit(enum nrf_edge e)
{
    if (nrf_trace_len < NRF_TRACE_MAX) {
        nrf_trace[nrf_trace_len] = e;
    }
    nrf_trace_len++;
}

void nrf_gpio_trace_reset(uint8_t scl_cfg, uint8_t sda_cfg)
{
    memset(nrf_trace, 0, sizeof nrf_trace);
    nrf_trace_len = 0;
    nrf_gpio_foreign_pin_writes = 0;
    s_scl_cfg = scl_cfg;
    s_sda_cfg = sda_cfg;
    s_bit_count = 0;
    s_bit_pos = 0;
    s_sda_released = false;
}

void nrf_gpio_set_sda_reads(const uint8_t *bits, unsigned count)
{
    if (count > sizeof s_bits) {
        count = (unsigned)sizeof s_bits;
    }
    memcpy(s_bits, bits, count);
    s_bit_count = count;
    s_bit_pos = 0;
}

/* ---- the od_gpio surface opendisplay_i2c.c drives ---- */

void od_gpio_configure_output(uint8_t pin_cfg, bool value)
{
    if (pin_cfg == s_scl_cfg) {
        emit(value ? NRF_EDGE_SCL_HIGH : NRF_EDGE_SCL_LOW);
    } else if (pin_cfg == s_sda_cfg) {
        s_sda_released = false;
        emit(value ? NRF_EDGE_SDA_HIGH : NRF_EDGE_SDA_LOW);
    } else {
        nrf_gpio_foreign_pin_writes++;
    }
}

void od_gpio_configure_input(uint8_t pin_cfg, bool pull_up, bool pull_down)
{
    (void)pull_up;
    (void)pull_down;
    if (pin_cfg == s_scl_cfg) {
        emit(NRF_EDGE_SCL_HIGH);          /* released: the pull-up takes it high */
    } else if (pin_cfg == s_sda_cfg) {
        s_sda_released = true;
        emit(NRF_EDGE_SDA_RELEASED);
    } else {
        nrf_gpio_foreign_pin_writes++;
    }
}

int od_gpio_read(uint8_t pin_cfg)
{
    if (pin_cfg == s_scl_cfg) {
        return 1;                          /* never stretched: this engine cannot be tested for it */
    }
    if (pin_cfg != s_sda_cfg) {
        nrf_gpio_foreign_pin_writes++;
        return 1;
    }
    if (s_sda_released && s_bit_pos < s_bit_count) {
        return s_bits[s_bit_pos++] ? 1 : 0;
    }
    return 1;                              /* floats high: NACK, or all-ones data */
}

void k_busy_wait(uint32_t us) { (void)us; }

/* Board-specific on the target (nrf52840 and nrf54 decode differently); od_i2c_init() calls it
 * only to reject an unusable pin. Anything but the absent sentinel decodes here, so the suite
 * exercises the adapter's own pin handling rather than a board's codec. */
bool od_pin_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out)
{
    if (cfg == 0xFFu) {
        return false;
    }
    *port_out = (uint8_t)(cfg >> 5);
    *pin_out = (uint8_t)(cfg & 0x1Fu);
    return true;
}

unsigned nrf_trace_count(enum nrf_edge e)
{
    unsigned n = 0;
    for (unsigned i = 0; i < nrf_trace_len && i < NRF_TRACE_MAX; ++i) {
        if (nrf_trace[i] == e) { n++; }
    }
    return n;
}

static unsigned scan(bool want_start)
{
    bool scl_high = false, sda_high = true;
    unsigned n = 0;

    for (unsigned i = 0; i < nrf_trace_len && i < NRF_TRACE_MAX; ++i) {
        switch (nrf_trace[i]) {
        case NRF_EDGE_SCL_HIGH: scl_high = true;  break;
        case NRF_EDGE_SCL_LOW:  scl_high = false; break;
        case NRF_EDGE_SDA_LOW:
            if (want_start && scl_high && sda_high) { n++; }
            sda_high = false;
            break;
        case NRF_EDGE_SDA_HIGH:
        case NRF_EDGE_SDA_RELEASED:
            if (!want_start && scl_high && !sda_high) { n++; }
            sda_high = true;
            break;
        default: break;
        }
    }
    return n;
}

unsigned nrf_trace_starts(void) { return scan(true); }
unsigned nrf_trace_stops(void)  { return scan(false); }
