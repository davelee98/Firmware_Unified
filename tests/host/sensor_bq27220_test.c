/* The shared BQ27220 driver, over the fake I2C wire.
 *
 * §4's list: two-byte voltage, one-byte SOC, enable ordering, TTL and cache behaviour, the
 * charging tri-state, and MSD packing. The bus instance is deliberately NOT 0 -- a driver that
 * ignored bus_id and hardcoded 0 passed the first version of the SHT40 suite.
 */

#include "od_sensor_bq27220.h"
#include "od_sensor_app.h"
#include "od_hal_i2c.h"
#include "od_config.h"
#include "fake_i2c/i2c_wire.h"

#include "od_check.h"

#include <string.h>

extern const struct od_config *i2c_ref_cfg;

#define BUS_INSTANCE 2u
#define GAUGE_ADDR   0x55u

static uint8_t  g_msd[11];
static uint32_t g_now_ms;
static unsigned g_enable_calls;
static bool     g_enable_last;
static bool     g_state_known;
static bool     g_state_level_high;

void od_sensor_app_delay_ms(uint16_t ms) { (void)ms; }
void od_sensor_app_msd_write(uint8_t i, uint8_t v) { if (i < 11u) { g_msd[i] = v; } }
void od_sensor_app_bus_recover(uint8_t b) { (void)b; }

/* The seam carries LEVELS now, so these fakes supply pin electricity and nothing else. A fake
 * that answered "charging" would be answering the question under test. */
bool od_sensor_app_bq_enable_drive(bool level_high)
{
    g_enable_calls++;
    g_enable_last = level_high;
    return true;
}
bool od_sensor_app_bq_state_level(bool *level_high)
{
    if (!g_state_known) { return false; }
    *level_high = g_state_level_high;
    return true;
}

static struct od_config g_cfg;

static void setup(uint8_t msd_start)
{
    g_now_ms += 60000u;                 /* past the TTL of whatever ran before */
    memset(&g_cfg, 0, sizeof g_cfg);
    memset(g_msd, 0xA5, sizeof g_msd);
    g_enable_calls = 0;
    g_state_known = false;
    g_state_level_high = false;
    g_cfg.data_bus_count = 1u;
    g_cfg.data_buses[0].instance_number = BUS_INSTANCE;
    g_cfg.data_buses[0].bus_type = 0x01u;
    g_cfg.data_buses[0].pin_1 = 10u;
    g_cfg.data_buses[0].pin_2 = 11u;
    g_cfg.sensor_count = 1u;
    g_cfg.sensors[0].sensor_type = OD_SENSOR_TYPE_BQ27220;
    g_cfg.sensors[0].bus_id = BUS_INSTANCE;
    g_cfg.sensors[0].msd_data_start_byte = msd_start;
    i2c_ref_cfg = &g_cfg;
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, GAUGE_ADDR);
}

/* The fake serves one read stream per address, so voltage and SOC come from the same bytes in
 * order: two for the voltage, then one for the SOC. */
static void script(uint16_t mv, uint8_t soc)
{
    uint8_t b[3];
    b[0] = (uint8_t)(mv & 0xFFu);
    b[1] = (uint8_t)(mv >> 8);
    b[2] = soc;
    i2c_wire_set_read_data(GAUGE_ADDR, b, 3);
}

static void test_voltage_and_soc_widths(void)
{
    CASE("voltage is two bytes little-endian, SOC is one");
    setup(4u);
    script(3700u, 77u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(od_sensor_bq27220_voltage_volts() > 3.69f);
    CHECK(od_sensor_bq27220_voltage_volts() < 3.71f);
    /* The exact byte, not the masked SOC: masking bit 7 would accept a spurious charging flag
     * on a fixture where charging is unknown. */
    CHECK(g_msd[4] == 77u);

    CASE("and each register is its own repeated-START transaction");
    /* Two selectors, two reads, no STOP inside either. A STOP between the selector and the read
     * makes this part answer as if unaddressed -- plausible garbage, not an error. */
    CHECK(i2c_wire_count(I2C_EV_RSTART) == 2u);
    CHECK(i2c_wire_count(I2C_EV_ADDR_W) == 2u);
    CHECK(i2c_wire_count(I2C_EV_ADDR_R) == 2u);

    CASE("BOTH selectors are sent, and the widths are 2 then 1");
    /* The fake picks its bytes by address and cursor, NOT by the selector, so a driver that sent
     * 0x08 twice would still see the right SOC value and pass a count-only test. */
    {
        uint8_t tx[8];
        unsigned n = 0;
        for (unsigned i = 0; i < i2c_wire_len; ++i) {
            if (i2c_wire_trace[i].ev == I2C_EV_TX && n < 8u) { tx[n++] = i2c_wire_trace[i].arg; }
        }
        CHECK(n == 2u);
        CHECK(tx[0] == 0x08u);        /* voltage */
        CHECK(tx[1] == 0x2Cu);        /* state of charge */
    }
    /* Two bytes for the voltage plus one for the SOC. Three, not four: a two-byte SOC read would
     * overflow a one-byte destination and still show the right value here. */
    CHECK(i2c_wire_count(I2C_EV_RX) == 3u);
}

/* One poll with a scripted gauge, a known STAT level and a charger_flags value. */
static uint8_t poll_with_state(uint8_t flags, bool level_high)
{
    setup(4u);
    script(3700u, 50u);
    g_cfg.power_option.charger_flags = flags;
    g_state_known = true;
    g_state_level_high = level_high;
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    return g_msd[4];
}

/* THE POLARITY IS THE CANONICAL HEADER'S, IN ALL FOUR COMBINATIONS.
 *
 * opendisplay_structs.h:482 -- OD_CHARGER_FLAG_STATE_ACTIVE_LOW is "charge-state (BQ25616 STAT)
 * is active-low: charging when LOW"; with the flag clear the pin is active-high. Every port used
 * to answer this backwards in BOTH branches, which is a plain inversion and therefore invisible
 * to any test that only checks one flag value. All four rows are here for that reason: flipping
 * the operator fails two of them, and swapping the flag's sense fails the other two.
 *
 * DIVERGENCE_MATRIX 21, FOLLOWUPS 19. */
static void test_charge_state_polarity(void)
{
    CASE("active-low flag set: LOW is charging");
    CHECK(poll_with_state(OD_CHARGER_FLAG_STATE_ACTIVE_LOW, false) == (50u | 0x80u));

    CASE("active-low flag set: HIGH is not charging");
    CHECK(poll_with_state(OD_CHARGER_FLAG_STATE_ACTIVE_LOW, true) == 50u);

    CASE("active-low flag clear: HIGH is charging");
    CHECK(poll_with_state(0u, true) == (50u | 0x80u));

    CASE("active-low flag clear: LOW is not charging");
    CHECK(poll_with_state(0u, false) == 50u);

    /* The enable flag is the neighbour that was always right; pinned so a shared owner cannot
     * regress it while fixing its sibling. */
    CASE("enable active-low asserts the enable pin LOW");
    setup(4u);
    g_cfg.power_option.charger_flags = OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW;
    od_sensor_bq27220_init(&g_cfg);
    CHECK(g_enable_calls == 1u);
    CHECK(g_enable_last == false);

    CASE("enable active-high asserts the enable pin HIGH");
    setup(4u);
    g_cfg.power_option.charger_flags = 0u;
    od_sensor_bq27220_init(&g_cfg);
    CHECK(g_enable_calls == 1u);
    CHECK(g_enable_last == true);
}

static void test_charging_tristate(void)
{
    CASE("UNKNOWN packs as not-charging, because the MSD has no third state");
    setup(4u);
    script(3700u, 50u);
    g_state_known = false;             /* no state pin */
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(g_msd[4] == 50u);
}

static void test_failure_invalidates_the_cache(void)
{
    CASE("a good read caches the voltage");
    setup(4u);
    script(4100u, 90u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(od_sensor_bq27220_voltage_volts() > 4.0f);

    CASE("a failed read invalidates the CACHE but leaves the MSD byte alone");
    setup(4u);
    g_msd[4] = 0x42u;                  /* a previous good sample */
    i2c_wire_reset();                  /* no device on the bus at all */
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(od_sensor_bq27220_voltage_volts() < 0.0f);
    /* Both donors return before the MSD write on a failed VOLTAGE read, so the advertisement
     * keeps the last good byte. SHT40 writes an invalid marker in the same situation; BQ27220
     * does not, and the two donors are inconsistent with each other. Preserved because it is
     * wire-visible and changing it is not this promotion's to authorise -- FOLLOWUPS 18. */
    CHECK(g_msd[4] == 0x42u);

    CASE("zero millivolts is a gauge fault, not a flat pack");
    setup(4u);
    script(0u, 50u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(od_sensor_bq27220_voltage_volts() < 0.0f);
    CHECK(g_msd[4] == 0xFFu);
}

static void test_soc_clamp_and_bad_soc(void)
{
    CASE("an SOC above 100 is clamped");
    setup(4u);
    script(3700u, 200u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK((g_msd[4] & 0x7Fu) == 100u);
}

static void test_ttl(void)
{
    uint32_t base;

    CASE("a second poll inside the TTL does nothing");
    setup(4u);
    script(3700u, 50u);
    base = g_now_ms;
    od_sensor_bq27220_poll(&g_cfg, base);
    unsigned after = i2c_wire_len;
    od_sensor_bq27220_poll(&g_cfg, base + 29999u);
    CHECK(i2c_wire_len == after);

    CASE("and one past it polls again");
    od_sensor_bq27220_poll(&g_cfg, base + 30000u);
    CHECK(i2c_wire_len > after);

    CASE("the TTL survives the 32-bit millisecond wrap");
    setup(4u);
    script(3700u, 50u);
    g_now_ms = 0xFFFFFF00u;
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    after = i2c_wire_len;
    od_sensor_bq27220_poll(&g_cfg, 0xFFFFFF00u + 20000u);   /* wraps past zero */
    CHECK(i2c_wire_len == after);
    od_sensor_bq27220_poll(&g_cfg, 0xFFFFFF00u + 30000u);
    CHECK(i2c_wire_len > after);
}

static void test_config_and_first_match(void)
{
    CASE("no gauge configured: nothing polled, and is_configured says so");
    setup(4u);
    g_cfg.sensor_count = 0u;
    CHECK(!od_sensor_bq27220_is_configured(&g_cfg));
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(i2c_wire_len == 0u);

    CASE("a configured gauge reports as configured");
    setup(4u);
    CHECK(od_sensor_bq27220_is_configured(&g_cfg));   /* an always-false impl passes without this */

    CASE("FIRST match only -- a second gauge entry is ignored, not polled too");
    setup(4u);
    g_cfg.sensor_count = 2u;
    g_cfg.sensors[1] = g_cfg.sensors[0];
    g_cfg.sensors[1].msd_data_start_byte = 8u;
    script(3700u, 50u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(g_msd[4] == 50u);            /* the FIRST gauge's value exactly */
    CHECK(g_msd[8] == 0xA5u);          /* the second did not */
    CHECK(i2c_wire_count(I2C_EV_ADDR_W) == 2u);   /* two registers, one gauge */

    CASE("bus_id 0xFF is not probed");
    setup(4u);
    g_cfg.sensors[0].bus_id = 0xFFu;
    g_msd[4] = 0x42u;
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(i2c_wire_len == 0u);
    CHECK(g_msd[4] == 0x42u);          /* same stale-byte behaviour: see FOLLOWUPS 18 */
}

static void test_init_establishes_the_charger(void)
{
    CASE("init drives the charger enable even with no gauge configured");
    setup(4u);
    g_cfg.sensor_count = 0u;
    od_sensor_bq27220_init(&g_cfg);
    CHECK(g_enable_calls == 1u);
    CHECK(g_enable_last);
    CHECK(i2c_wire_len == 0u);         /* nothing to probe */

    CASE("and probes the gauge when one is configured");
    setup(4u);
    script(3700u, 50u);
    od_sensor_bq27220_init(&g_cfg);
    CHECK(g_enable_calls == 1u);
    CHECK(i2c_wire_count(I2C_EV_ADDR_W) == 1u);   /* one voltage read */

    CASE("an unassigned bus is not probed, but the charger is still established");
    setup(4u);
    g_cfg.sensors[0].bus_id = 0xFFu;
    od_sensor_bq27220_init(&g_cfg);
    CHECK(g_enable_calls == 1u);
    CHECK(i2c_wire_len == 0u);
}

static void test_more_edges(void)
{
    CASE("a failed SOC read after a good voltage still writes the invalid marker");
    setup(4u);
    {
        /* Two bytes only: the voltage read consumes them, the SOC read has nothing and the fake
         * reports the address absent for a second transaction it cannot serve. */
        uint8_t b[2] = { 0x74u, 0x0Eu };   /* 3700 mV */
        i2c_wire_set_read_data(GAUGE_ADDR, b, 2);
    }
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(od_sensor_bq27220_voltage_volts() > 3.6f);   /* the voltage still cached */

    CASE("an msd_data_start_byte past the block writes nothing");
    setup(11u);
    script(3700u, 50u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    for (unsigned i = 0; i < 11u; ++i) { CHECK(g_msd[i] == 0xA5u); }

    CASE("i2c_addr_7bit 0 and 0xFF both mean the default 0x55");
    for (unsigned k = 0; k < 2u; ++k) {
        setup(4u);
        g_cfg.sensors[0].i2c_addr_7bit = (k == 0u) ? 0x00u : 0xFFu;
        script(3700u, 50u);
        od_sensor_bq27220_poll(&g_cfg, g_now_ms);
        CHECK(i2c_wire_trace[1].ev == I2C_EV_ADDR_W && i2c_wire_trace[1].arg == 0x55u);
    }

    CASE("a configured non-default address is used as given");
    setup(4u);
    g_cfg.sensors[0].i2c_addr_7bit = 0x56u;
    i2c_wire_reset();
    i2c_wire_add_device(10, 11, 0x56u);
    script(3700u, 50u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(i2c_wire_trace[1].ev == I2C_EV_ADDR_W && i2c_wire_trace[1].arg == 0x56u);

    CASE("a FAILED poll still consumes the TTL");
    setup(4u);
    i2c_wire_reset();                  /* nothing on the bus */
    {
        uint32_t base = g_now_ms;
        od_sensor_bq27220_poll(&g_cfg, base);
        unsigned after = i2c_wire_len;
        od_sensor_bq27220_poll(&g_cfg, base + 1000u);
        CHECK(i2c_wire_len == after);  /* the latch is set before the read, as the donors did */
    }

    CASE("init neither validates nor clears the cached reading");
    setup(4u);
    script(4100u, 90u);
    od_sensor_bq27220_poll(&g_cfg, g_now_ms);
    CHECK(od_sensor_bq27220_voltage_volts() > 4.0f);
    od_sensor_bq27220_init(&g_cfg);
    CHECK(od_sensor_bq27220_voltage_volts() > 4.0f);   /* T4: a reload is not a state reset */
}

int main(void)
{
    test_more_edges();
    test_voltage_and_soc_widths();
    test_charging_tristate();
    test_charge_state_polarity();
    test_failure_invalidates_the_cache();
    test_soc_clamp_and_bad_soc();
    test_ttl();
    test_config_and_first_match();
    test_init_establishes_the_charger();
    return OD_CHECK_REPORT_NONEMPTY("sensor_bq27220", 56u);
}
