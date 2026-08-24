/* The shared SHT40 driver, over the fake I2C wire.
 *
 * What it pins, per PLAN_SENSOR_SEAM_2026-08-23.md § 4: reset and fallback addresses, the
 * write / delay / read ORDERING (two transactions with a STOP between, not a repeated START),
 * both CRC-8 words independently, the conversion and clamps, the poll TTL, the failure output,
 * and the two-pass retry the Nordic port had dropped.
 */

#include "od_sensor_sht40.h"
#include "od_sensor_app.h"
#include "od_hal_i2c.h"
#include "od_config.h"
#include "fake_i2c/i2c_wire.h"

#include "od_check.h"

#include <string.h>

/* ---- the seams the driver needs ---- */
extern const struct od_config *i2c_ref_cfg;

static uint8_t  g_msd[11];
static unsigned g_delays[16];
static unsigned g_delay_count;
static unsigned g_recover_count;
static uint32_t g_now_ms;

void od_sensor_app_delay_ms(uint16_t ms)
{
    if (g_delay_count < 16u) { g_delays[g_delay_count] = ms; }
    g_delay_count++;
    /* A delay marks the wire, so "the read came after the wait" is checkable. */
    i2c_wire_mark_delay();
}
void od_sensor_app_msd_write(uint8_t i, uint8_t v) { if (i < 11u) { g_msd[i] = v; } }
void od_sensor_app_bus_recover(uint8_t bus_id) { (void)bus_id; g_recover_count++; }
void od_hal_delay_us(uint32_t us) { (void)us; }

/* ---- fixtures ---- */
static struct od_config g_cfg;

static uint8_t crc8(const uint8_t *d, uint8_t n)
{
    uint8_t c = 0xFFu;
    for (uint8_t i = 0; i < n; i++) {
        c ^= d[i];
        for (uint8_t b = 8; b > 0; b--) {
            if ((c & 0x80u) != 0u) { c = (uint8_t)((c << 1) ^ 0x31u); }
            else { c = (uint8_t)(c << 1); }
        }
    }
    return c;
}

/* raw_t = 0x6666 -> -45 + 175*0.4 = 25.0 C ; raw_rh = 0x8000 -> -6 + 125*0.50000 = 56.5 % */
static void script_sample(uint8_t addr, uint16_t raw_t, uint16_t raw_rh,
                          bool break_crc1, bool break_crc2)
{
    uint8_t b[6];
    b[0] = (uint8_t)(raw_t >> 8); b[1] = (uint8_t)raw_t; b[2] = crc8(b, 2);
    b[3] = (uint8_t)(raw_rh >> 8); b[4] = (uint8_t)raw_rh; b[5] = crc8(b + 3, 2);
    if (break_crc1) { b[2] ^= 0xFFu; }
    if (break_crc2) { b[5] ^= 0xFFu; }
    i2c_wire_set_read_data(addr, b, 6);
}

/* The driver's poll TTL is a static that deliberately survives an init -- T4 says a config
 * reload must not silently reset it -- so every fixture moves time past it, or the second case
 * in a run is skipped and its assertions nothing. */
static void setup(uint8_t addr, uint8_t msd_start)
{
    g_now_ms += 60000u;
    memset(&g_cfg, 0, sizeof g_cfg);
    memset(g_msd, 0, sizeof g_msd);
    g_delay_count = 0;
    g_recover_count = 0;
    g_cfg.data_bus_count = 1u;
    g_cfg.data_buses[0].instance_number = 0u;
    g_cfg.data_buses[0].bus_type = 0x01u;
    g_cfg.data_buses[0].pin_1 = 10u;
    g_cfg.data_buses[0].pin_2 = 11u;
    g_cfg.sensor_count = 1u;
    g_cfg.sensors[0].sensor_type = OD_SENSOR_TYPE_SHT40;
    g_cfg.sensors[0].bus_id = 0u;
    g_cfg.sensors[0].i2c_addr_7bit = addr;
    g_cfg.sensors[0].msd_data_start_byte = msd_start;
    i2c_ref_cfg = &g_cfg;
    i2c_wire_reset();
}

static void test_measurement_ordering(void)
{
    CASE("command and read are two transactions with the wait between them");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x44u);
    script_sample(0x44u, 0x6666u, 0x8000u, false, false);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);

    /* Two STARTs, two STOPs, no repeated START: the part needs the conversion time, and a
     * repeated-START read would not give it any. */
    CHECK(i2c_wire_count(I2C_EV_START) == 2u);
    CHECK(i2c_wire_count(I2C_EV_STOP) == 2u);
    CHECK(i2c_wire_count(I2C_EV_RSTART) == 0u);
    CHECK(i2c_wire_trace[1].ev == I2C_EV_ADDR_W && i2c_wire_trace[1].arg == 0x44u);
    CHECK(i2c_wire_trace[2].ev == I2C_EV_TX && i2c_wire_trace[2].arg == 0xFDu);

    CASE("and the wait is 12 ms, and falls between them");
    CHECK(g_delay_count >= 1u);
    CHECK(g_delays[0] == 12u);
    CHECK(i2c_wire_delay_between_transactions());
}

static void test_conversion_and_msd(void)
{
    uint32_t v;

    CASE("conversion and MSD packing");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x44u);
    script_sample(0x44u, 0x6666u, 0x8000u, false, false);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);

    v = (uint32_t)g_msd[7] | ((uint32_t)g_msd[8] << 8) | ((uint32_t)g_msd[9] << 16);
    /* 25.0 C -> t_deci 250 -> tu 650 ; 56.5 % -> rh_deci 565 */
    CHECK((v & 0x3FFu) == 565u);
    CHECK(((v >> 10) & 0x7FFu) == 650u);
    CHECK(g_msd[10] == 0u);

    CASE("humidity is clamped to 0..100 %");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x44u);
    script_sample(0x44u, 0x6666u, 0xFFFFu, false, false);   /* -6 + 125 = 119 % */
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    v = (uint32_t)g_msd[7] | ((uint32_t)g_msd[8] << 8) | ((uint32_t)g_msd[9] << 16);
    CHECK((v & 0x3FFu) == 1000u);
}

static void test_both_crc_words(void)
{
    CASE("a bad temperature CRC rejects the sample");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x44u);
    script_sample(0x44u, 0x6666u, 0x8000u, true, false);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(g_msd[7] == 0xFFu && g_msd[8] == 0xFFu && g_msd[9] == 0xFFu);

    CASE("and so does a bad humidity CRC -- the second word is checked independently");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x44u);
    script_sample(0x44u, 0x6666u, 0x8000u, false, true);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(g_msd[7] == 0xFFu && g_msd[8] == 0xFFu && g_msd[9] == 0xFFu);
}

static void test_address_fallback_and_retry(void)
{
    CASE("a part at 0x45 is found though the config says 0x44");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x45u);
    script_sample(0x45u, 0x6666u, 0x8000u, false, false);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(g_msd[7] != 0xFFu || g_msd[8] != 0xFFu);

    CASE("nothing anywhere: TWO passes, with a bus recovery between them");
    /* The authority retries after recovering the bus; the Nordic port had dropped the second
     * pass, so a sensor that needed a recovery never got one. */
    setup(0x44u, 7u);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(g_recover_count == 1u);
    CHECK(g_msd[7] == 0xFFu && g_msd[8] == 0xFFu && g_msd[9] == 0xFFu);
}

static void test_ttl_and_wrap(void)
{
    CASE("a second poll inside the TTL does nothing");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x44u);
    script_sample(0x44u, 0x6666u, 0x8000u, false, false);
    uint32_t base = g_now_ms;
    unsigned after_first;

    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    after_first = i2c_wire_len;

    /* Relative to the poll that just happened. Absolute times would run BACKWARDS as earlier
     * fixtures advance the clock, and an unsigned now-minus-last of nearly 2^32 reads as "long
     * past the TTL" -- which samples, and looks like the TTL failing. */
    g_now_ms = base + 29999u;
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(i2c_wire_len == after_first);

    CASE("and one past it samples again");
    g_now_ms = base + 30000u;
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(i2c_wire_len > after_first);
}

static void test_unassigned_bus_and_multi_instance(void)
{
    CASE("bus_id 0xFF is not probed, and writes the invalid marker");
    setup(0x44u, 7u);
    g_cfg.sensors[0].bus_id = 0xFFu;
    i2c_wire_add_device(10, 11, 0x44u);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(i2c_wire_len == 0u);
    CHECK(g_msd[7] == 0xFFu);
    CHECK(g_recover_count == 0u);   /* refused before the retry loop, so no recovery either */

    CASE("every configured SHT40 is polled, not just the first");
    setup(0x44u, 0u);                /* 0 -> default start 7 */
    g_cfg.sensor_count = 2u;
    g_cfg.sensors[1] = g_cfg.sensors[0];
    g_cfg.sensors[1].msd_data_start_byte = 3u;
    i2c_wire_add_device(10, 11, 0x44u);
    script_sample(0x44u, 0x6666u, 0x8000u, false, false);
    od_sensor_sht40_poll(&g_cfg, g_now_ms);
    CHECK(g_msd[7] != 0u);
    CHECK(g_msd[3] != 0u);           /* the second instance landed at its own offset */
}

static void test_init_soft_reset(void)
{
    CASE("init soft-resets the configured address");
    setup(0x44u, 7u);
    i2c_wire_add_device(10, 11, 0x44u);
    od_sensor_sht40_init(&g_cfg);
    CHECK(i2c_wire_count(I2C_EV_TX) >= 1u);
    CHECK(i2c_wire_trace[2].ev == I2C_EV_TX && i2c_wire_trace[2].arg == 0x94u);

    CASE("and falls back to both factory addresses when that is not where it answers");
    setup(0x44u, 7u);                /* nothing on the bus at all */
    od_sensor_sht40_init(&g_cfg);
    CHECK(i2c_wire_count(I2C_EV_ADDR_W) == 3u);   /* configured, then 0x44, then 0x45 */

    CASE("an unassigned sensor is not reset either");
    setup(0x44u, 7u);
    g_cfg.sensors[0].bus_id = 0xFFu;
    od_sensor_sht40_init(&g_cfg);
    CHECK(i2c_wire_len == 0u);
}

int main(void)
{
    test_measurement_ordering();
    test_conversion_and_msd();
    test_both_crc_words();
    test_address_fallback_and_retry();
    test_ttl_and_wrap();
    test_unassigned_bus_and_multi_instance();
    test_init_soft_reset();
    return OD_CHECK_REPORT_NONEMPTY("sensor_sht40", 28u);
}
