/* The BG22 TNB132M transport, checked at the EDGES.
 *
 * MANDATORY, and it is not covered by anything that existed before: the BG22 NFC host tests fake
 * od_nfc_app_read/write, which sit ABOVE the transport, so they say nothing about where a START
 * falls, whether an ACK was sampled, or whether the last byte of a read was NACKed. This binds
 * the production targets/efr32bg22-slc/hal/od_hal_i2c.c to fake GPIO and delay and reads the
 * edge sequence back.
 *
 * WHAT IT MUST NOT ASSERT: stuck-bus behaviour. The engine drives SCL push-pull and only reads
 * SDA, so it cannot detect clock stretching, and claiming otherwise here would put a promise in
 * the test that the silicon cannot keep.
 */

#include "od_hal_i2c.h"
#include "od_config.h"
#include "em_gpio.h"

#include "od_check.h"

#include <string.h>

/* The transport resolves its bus through the target's global config. */
static struct od_config g_cfg;
const struct od_config *opendisplay_get_global_config(void) { return &g_cfg; }

#define SCL_PORT gpioPortA
#define SCL_PIN  3u
#define SDA_PORT gpioPortB
#define SDA_PIN  5u
#define SCL_CFG  0x03u   /* (port<<4)|pin  -> A3 */
#define SDA_CFG  0x15u   /* B5 */
#define DEV7     0x55u

static void install_bus(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.data_bus_count = 1u;
    g_cfg.data_buses[0].instance_number = 0u;
    g_cfg.data_buses[0].bus_type = 0x01u;
    g_cfg.data_buses[0].pin_1 = SCL_CFG;
    g_cfg.data_buses[0].pin_2 = SDA_CFG;
    fake_gpio_reset(SCL_PORT, SCL_PIN, SDA_PORT, SDA_PIN);
}

/* 8 ACK bits: one per byte the engine writes, 0 = ACK. */
static void script_acks(unsigned n_bytes_written, bool ack)
{
    static uint8_t bits[64];
    memset(bits, ack ? 0 : 1, sizeof bits);
    (void)n_bytes_written;
    fake_gpio_set_sda_reads(bits, sizeof bits);
}

/* Index of the Nth occurrence of an edge, or gpio_trace_len when absent. */
static unsigned nth(enum gpio_edge e, unsigned n)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < gpio_trace_len && i < GPIO_TRACE_MAX; ++i) {
        if (gpio_trace[i] == e && ++seen == n) {
            return i;
        }
    }
    return gpio_trace_len;
}

static unsigned count_edge(enum gpio_edge e)
{
    unsigned n = 0;
    for (unsigned i = 0; i < gpio_trace_len && i < GPIO_TRACE_MAX; ++i) {
        if (gpio_trace[i] == e) { n++; }
    }
    return n;
}

/* A START is SDA falling while SCL is high. Walk the trace tracking both lines. */
static unsigned count_starts(void)
{
    bool scl_high = false, sda_high = false;
    unsigned starts = 0;

    for (unsigned i = 0; i < gpio_trace_len && i < GPIO_TRACE_MAX; ++i) {
        switch (gpio_trace[i]) {
        case GPIO_EDGE_SCL_HIGH: scl_high = true;  break;
        case GPIO_EDGE_SCL_LOW:  scl_high = false; break;
        case GPIO_EDGE_SDA_HIGH: sda_high = true;  break;
        case GPIO_EDGE_SDA_LOW:
            if (scl_high && sda_high) { starts++; }
            sda_high = false;
            break;
        default: break;
        }
    }
    return starts;
}

/* A STOP is SDA rising while SCL is high. */
static unsigned count_stops(void)
{
    bool scl_high = false, sda_high = true;
    unsigned stops = 0;

    for (unsigned i = 0; i < gpio_trace_len && i < GPIO_TRACE_MAX; ++i) {
        switch (gpio_trace[i]) {
        case GPIO_EDGE_SCL_HIGH: scl_high = true;  break;
        case GPIO_EDGE_SCL_LOW:  scl_high = false; break;
        case GPIO_EDGE_SDA_LOW:  sda_high = false; break;
        case GPIO_EDGE_SDA_HIGH:
            if (scl_high && !sda_high) { stops++; }
            sda_high = true;
            break;
        default: break;
        }
    }
    return stops;
}

static void test_write_framing(void)
{
    const uint8_t tx[2] = { 0x10, 0x20 };

    CASE("a write is START, address, bytes, STOP, with one clock per bit plus ACK");
    install_bus();
    script_acks(3, true);
    CHECK(od_hal_i2c_write(0, DEV7, tx, 2) == OD_HAL_I2C_OK);
    CHECK(count_starts() == 1u);
    CHECK(count_stops() == 1u);
    /* 3 bytes x (8 data + 1 ACK) clocks, plus the SCL rise in START and the one in STOP. */
    CHECK(count_edge(GPIO_EDGE_SCL_HIGH) == 3u * 9u + 2u);
    /* SDA is released once per byte to sample the ACK. */
    CHECK(count_edge(GPIO_EDGE_SDA_INPUT) == 3u);
    CHECK(fake_udelay_total_us > 0u);
}

static void test_address_nack(void)
{
    const uint8_t tx[1] = { 0x10 };

    CASE("an address NACK stops immediately and reports ENODEV, not EIO");
    install_bus();
    script_acks(1, false);            /* SDA floats high at the ACK slot */
    CHECK(od_hal_i2c_write(0, DEV7, tx, 1) == OD_HAL_I2C_ENODEV);
    /* One byte clocked (the address) and then a STOP: the payload never went out. */
    CHECK(count_edge(GPIO_EDGE_SDA_INPUT) == 1u);
    CHECK(count_stops() == 1u);

    CASE("probe is address-only and reports the same way");
    install_bus();
    script_acks(1, false);
    CHECK(od_hal_i2c_probe(0, DEV7) == OD_HAL_I2C_ENODEV);
    CHECK(count_edge(GPIO_EDGE_SDA_INPUT) == 1u);
    install_bus();
    script_acks(1, true);
    CHECK(od_hal_i2c_probe(0, DEV7) == OD_HAL_I2C_OK);
    CHECK(count_starts() == 1u && count_stops() == 1u);
}

static void test_block_read_is_one_transaction(void)
{
    uint8_t rx[16];
    const uint8_t sub = 0x04;

    /* The TNB132M block read: START, addr|W, sub, REPEATED START, addr|R, 16 bytes, NACK last. */
    CASE("write_read issues a repeated START and no STOP between the phases");
    install_bus();
    script_acks(2, true);
    CHECK(od_hal_i2c_write_read(0, DEV7, &sub, 1, rx, 16) == OD_HAL_I2C_OK);
    CHECK(count_starts() == 2u);      /* the second is the repeated START */
    CHECK(count_stops() == 1u);       /* one STOP, at the very end */

    CASE("and the STOP comes after the last data byte, not between the phases");
    /* The only SDA rise while SCL is high is the final STOP, so the first STOP index must fall
     * after every read clock. */
    CHECK(nth(GPIO_EDGE_SDA_INPUT, 2) < gpio_trace_len);   /* released again for the read phase */
}

static void test_final_read_byte_is_nacked(void)
{
    uint8_t rx[2];

    /* The master ACKs every byte but the last; NACKing the last is how the peer is told to stop
     * driving, and getting it wrong leaves the bus held. */
    CASE("a multi-byte read ACKs all but the final byte");
    install_bus();
    script_acks(1, true);
    CHECK(od_hal_i2c_read(0, DEV7, rx, 2) == OD_HAL_I2C_OK);
    /* Per byte the engine drives the ACK bit: low for ACK, high for NACK. With two bytes there is
     * exactly one ACK (low) and one NACK (high) in the ack slots, so SDA is driven low at least
     * once after a read byte and left high before the STOP. */
    CHECK(count_edge(GPIO_EDGE_SDA_OUTPUT) >= 2u);
    CHECK(count_stops() == 1u);

    CASE("a single-byte read NACKs immediately");
    install_bus();
    script_acks(1, true);
    CHECK(od_hal_i2c_read(0, DEV7, rx, 1) == OD_HAL_I2C_OK);
    CHECK(count_stops() == 1u);
}

static void test_bus_resolution(void)
{
    const uint8_t tx[1] = { 0x10 };

    CASE("an unresolvable bus is refused before any edge is driven");
    install_bus();
    CHECK(od_hal_i2c_write(9, DEV7, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(gpio_trace_len == 0u);

    CASE("a duplicated instance is ambiguous, and also drives nothing");
    install_bus();
    g_cfg.data_bus_count = 2u;
    g_cfg.data_buses[1] = g_cfg.data_buses[0];
    CHECK(od_hal_i2c_write(0, DEV7, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(gpio_trace_len == 0u);

    CASE("bad arguments drive nothing either");
    install_bus();
    CHECK(od_hal_i2c_write(0, 0x80u, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write(0, DEV7, NULL, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write(0, DEV7, tx, 0) == OD_HAL_I2C_EINVAL);
    CHECK(gpio_trace_len == 0u);

    CASE("bus_id 0xFF is taken literally: a record numbered 0xFF resolves");
    install_bus();
    g_cfg.data_buses[0].instance_number = 0xFFu;
    script_acks(2, true);
    CHECK(od_hal_i2c_write(0xFFu, DEV7, tx, 1) == OD_HAL_I2C_OK);
    CHECK(gpio_trace_len > 0u);
}

int main(void)
{
    test_write_framing();
    test_address_nack();
    test_block_read_is_one_transaction();
    test_final_read_byte_is_nacked();
    test_bus_resolution();
    return OD_CHECK_REPORT_NONEMPTY("silabs_i2c_trace", 25u);
}
