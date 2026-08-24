#include "i2c_wire.h"

#include <string.h>

struct i2c_event i2c_wire_trace[I2C_WIRE_MAX_EVENTS];
unsigned         i2c_wire_len;
uint8_t          i2c_wire_last_scl;
uint8_t          i2c_wire_last_sda;

struct dev {
    uint8_t scl, sda, addr7;
    uint8_t data[16];
    uint8_t data_len;
    uint8_t read_pos;
    bool    used;
};

static struct dev s_devs[8];
static bool       s_in_transaction;
static bool       s_fail_data;
static struct dev *s_active;

static void emit(enum i2c_ev ev, uint8_t arg)
{
    if (i2c_wire_len < I2C_WIRE_MAX_EVENTS) {
        i2c_wire_trace[i2c_wire_len].ev = ev;
        i2c_wire_trace[i2c_wire_len].arg = arg;
    }
    i2c_wire_len++;
}

void i2c_wire_reset(void)
{
    memset(i2c_wire_trace, 0, sizeof i2c_wire_trace);
    memset(s_devs, 0, sizeof s_devs);
    i2c_wire_len = 0;
    i2c_wire_last_scl = 0xFF;
    i2c_wire_last_sda = 0xFF;
    s_in_transaction = false;
    s_fail_data = false;
    s_active = NULL;
}

void i2c_wire_add_device(uint8_t scl, uint8_t sda, uint8_t addr7)
{
    for (unsigned i = 0; i < sizeof s_devs / sizeof s_devs[0]; ++i) {
        if (!s_devs[i].used) {
            s_devs[i].used = true;
            s_devs[i].scl = scl;
            s_devs[i].sda = sda;
            s_devs[i].addr7 = addr7;
            return;
        }
    }
}

void i2c_wire_set_read_data(uint8_t addr7, const uint8_t *data, uint8_t len)
{
    for (unsigned i = 0; i < sizeof s_devs / sizeof s_devs[0]; ++i) {
        if (s_devs[i].used && s_devs[i].addr7 == addr7) {
            if (len > sizeof s_devs[i].data) {
                len = (uint8_t)sizeof s_devs[i].data;
            }
            memcpy(s_devs[i].data, data, len);
            s_devs[i].data_len = len;
            s_devs[i].read_pos = 0;
            return;
        }
    }
}

void i2c_wire_fail_next_data(void) { s_fail_data = true; }

bool i2c_wire_start(uint8_t scl, uint8_t sda)
{
    if (scl == 0xFF || sda == 0xFF || scl == sda) {
        return false;
    }
    /* A START with no preceding STOP is a REPEATED start, and the distinction is the whole
     * point of this fake. */
    emit(s_in_transaction ? I2C_EV_RSTART : I2C_EV_START, 0);
    s_in_transaction = true;
    i2c_wire_last_scl = scl;
    i2c_wire_last_sda = sda;
    return true;
}

void i2c_wire_stop(void)
{
    emit(I2C_EV_STOP, 0);
    s_in_transaction = false;
    s_active = NULL;
}

bool i2c_wire_addr(uint8_t addr7, bool read)
{
    emit(read ? I2C_EV_ADDR_R : I2C_EV_ADDR_W, addr7);
    s_active = NULL;
    for (unsigned i = 0; i < sizeof s_devs / sizeof s_devs[0]; ++i) {
        if (s_devs[i].used && s_devs[i].addr7 == addr7 &&
            s_devs[i].scl == i2c_wire_last_scl && s_devs[i].sda == i2c_wire_last_sda) {
            s_active = &s_devs[i];
            return true;
        }
    }
    return false;   /* address NACK -- nothing at that address on THIS bus */
}

bool i2c_wire_tx(uint8_t byte)
{
    emit(I2C_EV_TX, byte);
    if (s_fail_data) { s_fail_data = false; return false; }
    return s_active != NULL;
}

bool i2c_wire_rx(uint8_t *byte)
{
    uint8_t v = 0;

    if (s_fail_data) { s_fail_data = false; emit(I2C_EV_RX, 0); return false; }
    if (s_active != NULL && s_active->data_len > 0u) {
        v = s_active->data[s_active->read_pos % s_active->data_len];
        s_active->read_pos++;
    }
    emit(I2C_EV_RX, v);
    *byte = v;
    return s_active != NULL;
}

enum i2c_ev i2c_wire_last_ev(void)
{
    unsigned n = i2c_wire_len;

    if (n == 0u) {
        return (enum i2c_ev)0;
    }
    if (n > I2C_WIRE_MAX_EVENTS) {
        n = I2C_WIRE_MAX_EVENTS;   /* emit() counts past the array; never read past it */
    }
    return i2c_wire_trace[n - 1u].ev;
}

bool i2c_wire_has_stop_between(unsigned from, unsigned to)
{
    for (unsigned i = from; i < to && i < i2c_wire_len; ++i) {
        if (i2c_wire_trace[i].ev == I2C_EV_STOP) {
            return true;
        }
    }
    return false;
}

unsigned i2c_wire_count(enum i2c_ev ev)
{
    unsigned n = 0;
    for (unsigned i = 0; i < i2c_wire_len && i < I2C_WIRE_MAX_EVENTS; ++i) {
        if (i2c_wire_trace[i].ev == ev) { n++; }
    }
    return n;
}
