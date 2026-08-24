/* Nordic's production I2C adapter, checked at the EDGES.
 *
 * The prerequisite the sensor-seam plan gained after step 4's review: an adapter that resolves a
 * bus and hands pins to an engine can swap SCL and SDA, and that compiles, links and passes every
 * other check in this repo. Here a swap is visible, because the trace is labelled by pin config
 * byte -- the same two bytes the DataBus record carries.
 *
 * Binds targets/nordic-zephyr/src/od_hal_i2c.c and src/opendisplay_i2c.c, the production files.
 */

#include "od_hal_i2c.h"
#include "od_config.h"
#include "od_gpio_trace.h"

#include "od_check.h"

#include <string.h>

static struct od_config g_cfg;
const struct od_config *opendisplay_get_global_config(void) { return &g_cfg; }

#define SCL_CFG 0x21u
#define SDA_CFG 0x0Au
#define DEV7    0x44u

static void install(uint32_t hz)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.data_bus_count = 1u;
    g_cfg.data_buses[0].instance_number = 0u;
    g_cfg.data_buses[0].bus_type = 0x01u;
    g_cfg.data_buses[0].pin_1 = SCL_CFG;   /* SCL */
    g_cfg.data_buses[0].pin_2 = SDA_CFG;   /* SDA */
    g_cfg.data_buses[0].bus_speed_hz = hz;
    nrf_gpio_trace_reset(SCL_CFG, SDA_CFG);
}

static void all_ack(void)
{
    static uint8_t bits[128];
    memset(bits, 0, sizeof bits);          /* every released-SDA sample reads 0 = ACK */
    nrf_gpio_set_sda_reads(bits, sizeof bits);
}

static void test_pins_are_not_swapped(void)
{
    const uint8_t tx[1] = { 0x11 };

    /* THE ROW THIS FILE EXISTS FOR. pin_1 is SCL and pin_2 is SDA. If the adapter passes them the
     * other way round, the clock edges land on the SDA config byte and this fails immediately. */
    CASE("SCL is clocked and SDA is not, so the two are not transposed");
    install(0);
    all_ack();
    CHECK(od_hal_i2c_write(0, DEV7, tx, 1) == OD_HAL_I2C_OK);
    /* Two bytes x 9 clocks: SCL toggles far more than SDA changes level. */
    CHECK(nrf_trace_count(NRF_EDGE_SCL_HIGH) >= 18u);
    CHECK(nrf_trace_count(NRF_EDGE_SCL_LOW) >= 18u);
    CHECK(nrf_gpio_foreign_pin_writes == 0u);

    CASE("and every edge lands on one of the two configured pins");
    CHECK(nrf_trace_len > 0u);
}

static void test_write_framing(void)
{
    const uint8_t tx[2] = { 0xA5, 0x5A };

    CASE("a write is one START ... STOP");
    install(0);
    all_ack();
    CHECK(od_hal_i2c_write(0, DEV7, tx, 2) == OD_HAL_I2C_OK);
    CHECK(nrf_trace_starts() == 1u);
    CHECK(nrf_trace_stops() == 1u);
}

static void test_write_read_is_one_transaction(void)
{
    const uint8_t sel = 0x08;
    uint8_t rx[2] = { 0, 0 };

    /* The BQ27220 / nPM1300 idiom. A STOP between the phases would make the device answer with
     * whatever an unaddressed read yields -- plausible values, not an error -- so the count of
     * STOPs is the assertion that matters. */
    CASE("write_read issues two STARTs and exactly one STOP");
    install(0);
    all_ack();
    CHECK(od_hal_i2c_write_read(0, DEV7, &sel, 1, rx, 2) == OD_HAL_I2C_OK);
    CHECK(nrf_trace_starts() == 2u);
    CHECK(nrf_trace_stops() == 1u);

    /* ONE INITIALISED BUS OBJECT SERVES BOTH PHASES, which is what T2 requires and what the
     * START/STOP counts alone cannot show: re-initialising between them idles both lines
     * mid-transaction, and it still yields two STARTs and one STOP. The only observable
     * difference is one extra clock rise and one extra SDA release, from od_i2c_init()'s
     * idling. These counts are MEASURED, not derived -- if the engine's edge sequence changes
     * legitimately they move together, and that is a prompt to re-measure, not to relax them. */
    CHECK(nrf_trace_count(NRF_EDGE_SCL_HIGH) == 49u);
    CHECK(nrf_trace_count(NRF_EDGE_SDA_RELEASED) == 18u);

    CASE("write() then read() is distinguishable: two STARTs and two STOPs");
    install(0);
    all_ack();
    CHECK(od_hal_i2c_write(0, DEV7, &sel, 1) == OD_HAL_I2C_OK);
    CHECK(od_hal_i2c_read(0, DEV7, rx, 2) == OD_HAL_I2C_OK);
    CHECK(nrf_trace_starts() == 2u);
    CHECK(nrf_trace_stops() == 2u);
}

static void test_address_nack(void)
{
    const uint8_t tx[1] = { 0x11 };
    static uint8_t nack[128];

    CASE("nothing answering the address stops the transaction and releases the bus");
    install(0);
    memset(nack, 1, sizeof nack);           /* SDA floats high at every ACK slot */
    nrf_gpio_set_sda_reads(nack, sizeof nack);
    CHECK(od_hal_i2c_write(0, DEV7, tx, 1) != OD_HAL_I2C_OK);
    CHECK(nrf_trace_stops() >= 1u);
}

static void test_bus_resolution(void)
{
    const uint8_t tx[1] = { 0x11 };
    uint8_t rx[1] = { 0 };

    CASE("an absent or ambiguous bus drives no edge at all");
    install(0);
    CHECK(od_hal_i2c_write(9, DEV7, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(nrf_trace_len == 0u);

    install(0);
    g_cfg.data_bus_count = 2u;
    g_cfg.data_buses[1] = g_cfg.data_buses[0];   /* instance 0 declared twice */
    CHECK(od_hal_i2c_read(0, DEV7, rx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(nrf_trace_len == 0u);

    CASE("bad arguments drive no edge either");
    install(0);
    CHECK(od_hal_i2c_write(0, 0x80u, tx, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write(0, DEV7, NULL, 1) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write(0, DEV7, tx, 0) == OD_HAL_I2C_EINVAL);
    CHECK(od_hal_i2c_write_read(0, DEV7, tx, 1, rx, 0) == OD_HAL_I2C_EINVAL);
    CHECK(nrf_trace_len == 0u);

    CASE("bus_id 0xFF is taken literally: a record numbered 0xFF resolves");
    install(0);
    g_cfg.data_buses[0].instance_number = 0xFFu;
    all_ack();
    CHECK(od_hal_i2c_write(0xFFu, DEV7, tx, 1) == OD_HAL_I2C_OK);
    CHECK(nrf_trace_len > 0u);
}

static void test_probe_is_address_only(void)
{
    CASE("probe drives an address and no data phase");
    install(0);
    all_ack();
    CHECK(od_hal_i2c_probe(0, DEV7) == OD_HAL_I2C_OK);
    CHECK(nrf_trace_starts() == 1u);
    CHECK(nrf_trace_stops() == 1u);
    /* One byte only: 9 clocks, not 18. */
    CHECK(nrf_trace_count(NRF_EDGE_SCL_HIGH) < 18u);
}

int main(void)
{
    test_pins_are_not_swapped();
    test_write_framing();
    test_write_read_is_one_transaction();
    test_address_nack();
    test_bus_resolution();
    test_probe_is_address_only();
    return OD_CHECK_REPORT_NONEMPTY("nordic_i2c_trace", 25u);
}
