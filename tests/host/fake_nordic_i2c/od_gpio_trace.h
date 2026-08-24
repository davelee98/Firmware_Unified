/* Edge-recording od_gpio + k_busy_wait, for binding Nordic's production I2C adapter.
 *
 * WHY EDGES AND NOT tests/host/i2c_contract.inc. That contract asserts on TRANSACTION tokens,
 * and this engine speaks GPIO. Decoding edges back into transactions means writing a software
 * I2C slave -- worth doing eventually, and reusable for BG22, but larger than the thing it is
 * checking. What is needed first is the class of defect that reached step 4 and passed the whole
 * gate: SCL and SDA swapped. An edge trace labelled BY PIN catches that, because a swap moves
 * every edge to the other line.
 */

#ifndef OD_TEST_FAKE_NORDIC_I2C_H
#define OD_TEST_FAKE_NORDIC_I2C_H

#include <stdbool.h>
#include <stdint.h>

enum nrf_edge {
    NRF_EDGE_SCL_HIGH = 1,
    NRF_EDGE_SCL_LOW,
    NRF_EDGE_SDA_HIGH,
    NRF_EDGE_SDA_LOW,
    NRF_EDGE_SDA_RELEASED,   /* configured as input: the master is listening */
};

#define NRF_TRACE_MAX 8192u
extern enum nrf_edge nrf_trace[NRF_TRACE_MAX];
extern unsigned      nrf_trace_len;

/* Which pin CONFIG BYTES are SCL and SDA. Passing the two the DataBus record carries is what
 * makes a swapped adapter visible: every edge lands on the wrong line. */
void nrf_gpio_trace_reset(uint8_t scl_cfg, uint8_t sda_cfg);

/* What SDA reads back while released, consumed in order. 0 at an ACK slot is an ACK. */
void nrf_gpio_set_sda_reads(const uint8_t *bits, unsigned count);

/* An edge was driven on a pin that is neither SCL nor SDA -- i.e. the adapter resolved the bus
 * to the wrong pins entirely. */
extern unsigned nrf_gpio_foreign_pin_writes;

unsigned nrf_trace_count(enum nrf_edge e);
/* START = SDA falling while SCL high; STOP = SDA rising while SCL high. */
unsigned nrf_trace_starts(void);
unsigned nrf_trace_stops(void);

#endif
