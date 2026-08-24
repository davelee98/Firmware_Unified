/* A fake I2C wire that records FRAMING, not bytes.
 *
 * The contract this exists to check is about where STARTs, repeated STARTs and STOPs fall --
 * "write ends with STOP", "write_read has no STOP between its phases". Recording a token per
 * bus event makes those assertions direct instead of inferred, and makes the difference between
 * write+read and write_read visible, which is the one an engine can get wrong while returning
 * the right bytes.
 */

#ifndef OD_TEST_FAKE_I2C_WIRE_H
#define OD_TEST_FAKE_I2C_WIRE_H

#include <stdbool.h>
#include <stdint.h>

enum i2c_ev {
    I2C_EV_START = 1,
    I2C_EV_RSTART,      /* a START issued with no preceding STOP */
    I2C_EV_STOP,
    I2C_EV_ADDR_W,      /* arg = 7-bit address */
    I2C_EV_ADDR_R,
    I2C_EV_TX,          /* arg = byte */
    I2C_EV_RX,          /* arg = byte */
};

struct i2c_event {
    enum i2c_ev ev;
    uint8_t     arg;
};

#define I2C_WIRE_MAX_EVENTS 256u
#define I2C_WIRE_MAX_PINS   64u

extern struct i2c_event i2c_wire_trace[I2C_WIRE_MAX_EVENTS];
extern unsigned         i2c_wire_len;

/* Which pins the last transaction was driven on -- how a test proves bus SELECTION, not just
 * that some bus was used. */
extern uint8_t i2c_wire_last_scl;
extern uint8_t i2c_wire_last_sda;

void i2c_wire_reset(void);

/* Script one device present at addr7 on the bus identified by these pins. Anything else NACKs
 * its address. */
void i2c_wire_add_device(uint8_t scl, uint8_t sda, uint8_t addr7);

/* The next byte(s) a read of addr7 yields, cycled. */
void i2c_wire_set_read_data(uint8_t addr7, const uint8_t *data, uint8_t len);

/* Make the next transaction fail mid-data (a stretch timeout or a data NACK -- indistinguishable
 * at this seam by design, see the header's note on why there is no stuck-bus code). */
void i2c_wire_fail_next_data(void);

/* ---- the primitives a reference or production engine drives ---- */
bool i2c_wire_start(uint8_t scl, uint8_t sda);   /* false only if pins are invalid */
void i2c_wire_stop(void);
bool i2c_wire_addr(uint8_t addr7, bool read);    /* false = address NACK */
bool i2c_wire_tx(uint8_t byte);                  /* false = data NACK / fault */
bool i2c_wire_rx(uint8_t *byte);

/* The last event, or 0 when nothing reached the wire.
 *
 * Assertions must be SAFE on an empty trace, because "the implementation never touched the bus"
 * is precisely the failure they exist to catch -- an assertion that indexes trace[len-1]
 * underflows and crashes instead of reporting it. */
enum i2c_ev i2c_wire_last_ev(void);

/* Helpers for assertions. */
bool     i2c_wire_has_stop_between(unsigned from, unsigned to);
unsigned i2c_wire_count(enum i2c_ev ev);

#endif /* OD_TEST_FAKE_I2C_WIRE_H */
