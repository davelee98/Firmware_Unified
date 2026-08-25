/* The shared GT911 driver against a register-addressed fake part.
 *
 * THE MSD EXPECTATIONS WERE WRITTEN BEFORE THE PACKING CODE, from the only normative description
 * of the format that exists -- the donor's comment, which py-opendisplay, the JavaScript decoder
 * and the iOS app each implement independently. There is no version field and the canonical
 * header defines only the block's width and offset, so a packing change breaks every deployed
 * host silently. Nothing in tools/check.sh covered this before today.
 *
 *   byte 0  low nibble  contact count 1..5 while touching
 *                       6 = released, with the last coordinates retained in bytes 1..4
 *                       0 = never touched, and then the whole 5-byte block is cleared
 *           high nibble track id
 *   byte 1  x low       byte 2  x high      (little-endian)
 *   byte 3  y low       byte 4  y high
 *
 * THE FAKE SUPPLIES HARDWARE, NOT POLICY. It models a register file, the two I2C framings and the
 * 16-bit pointer, and it can be told which pointer byte order it will answer -- so the test can
 * ask which order the driver tried FIRST rather than only whether it eventually bound.
 */

#include "od_touch_gt911.h"

#include "od_adv_app.h"
#include "od_config.h"
#include "od_hal_i2c.h"
#include "od_touch_app.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_fails;
static const char *g_case = "";

#define CASE(name) do { g_case = (name); } while (0)
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
    } \
} while (0)

/* ------------------------------------------------------------------ the fake GT911 */

#define GT911_REG_BASE 0x8140u
#define GT911_REG_SPAN 0x60u

struct fake_part {
    bool     present;
    uint8_t  addr7;
    bool     answers_high_first;   /* which pointer byte order this part accepts */
    bool     answers_low_first;
    uint8_t  regs[GT911_REG_SPAN];
    uint16_t pointer;
    unsigned status_clears;        /* writes of 0 to 0x814E */
    unsigned reads;
    bool     fail_all;
};

static struct fake_part g_part;
static uint8_t  g_bus_ready_id = 0xFFu;   /* which bus od_touch_app_bus_prepare accepts */

/* Ordered log of what the driver asked the part, so ORDER is assertable and not inferred. */
#define TRACE_MAX 64
static char     g_trace[TRACE_MAX][32];
static unsigned g_trace_len;

static void trace(const char *fmt, unsigned a, unsigned b)
{
    if (g_trace_len < TRACE_MAX) {
        snprintf(g_trace[g_trace_len], sizeof g_trace[0], fmt, a, b);
        g_trace_len++;
    }
}

static bool trace_has(const char *s)
{
    unsigned i;

    for (i = 0; i < g_trace_len; i++) {
        if (strcmp(g_trace[i], s) == 0) {
            return true;
        }
    }
    return false;
}

static int trace_index(const char *s)
{
    unsigned i;

    for (i = 0; i < g_trace_len; i++) {
        if (strcmp(g_trace[i], s) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool set_pointer(uint8_t addr7, const uint8_t *sel, uint16_t n)
{
    uint16_t hi_first;
    uint16_t lo_first;

    if (!g_part.present || g_part.fail_all || addr7 != g_part.addr7 || n < 2u) {
        return false;
    }
    hi_first = (uint16_t)(((uint16_t)sel[0] << 8) | sel[1]);
    lo_first = (uint16_t)(((uint16_t)sel[1] << 8) | sel[0]);

    if (g_part.answers_high_first && hi_first >= GT911_REG_BASE &&
        hi_first < GT911_REG_BASE + GT911_REG_SPAN) {
        g_part.pointer = hi_first;
        trace("ptr:BE:%04X", hi_first, 0);
        return true;
    }
    if (g_part.answers_low_first && lo_first >= GT911_REG_BASE &&
        lo_first < GT911_REG_BASE + GT911_REG_SPAN) {
        g_part.pointer = lo_first;
        trace("ptr:LE:%04X", lo_first, 0);
        return true;
    }
    /* A real part ACKs a pointer write to any address; it simply has nothing useful there. That
     * is modelled by accepting the transaction and parking the pointer out of range, which is
     * what makes the byte-order probe cost ONE transaction rather than a retry cascade. */
    g_part.pointer = 0xFFFFu;
    trace("ptr:junk:%02X%02X", sel[0], sel[1]);   /* raw selector, so the ORDER tried is visible */
    return true;
}

int od_hal_i2c_write(uint8_t bus_id, uint8_t addr7, const uint8_t *data, uint16_t len)
{
    (void)bus_id;
    if (!g_part.present || g_part.fail_all || addr7 != g_part.addr7) {
        return OD_HAL_I2C_ENODEV;
    }
    if (len == 3u) {                      /* pointer + one payload byte: a register write */
        if (!set_pointer(addr7, data, 2u)) {
            return OD_HAL_I2C_EIO;
        }
        if (g_part.pointer == 0x814Eu && data[2] == 0u) {
            g_part.status_clears++;
            g_part.regs[0x814Eu - GT911_REG_BASE] = 0u;
            trace("clear:status", 0, 0);
        }
        return OD_HAL_I2C_OK;
    }
    return set_pointer(addr7, data, len) ? OD_HAL_I2C_OK : OD_HAL_I2C_EIO;
}

int od_hal_i2c_read(uint8_t bus_id, uint8_t addr7, uint8_t *data, uint16_t len)
{
    uint16_t i;

    (void)bus_id;
    if (!g_part.present || g_part.fail_all || addr7 != g_part.addr7) {
        return OD_HAL_I2C_ENODEV;
    }
    if (g_part.pointer == 0xFFFFu) {
        memset(data, 0, len);             /* out of range: real silicon returns junk, not an error */
        return OD_HAL_I2C_OK;
    }
    for (i = 0; i < len; i++) {
        uint16_t off = (uint16_t)(g_part.pointer - GT911_REG_BASE + i);

        data[i] = (off < GT911_REG_SPAN) ? g_part.regs[off] : 0u;
    }
    g_part.reads++;
    trace("read:%04X:%u", g_part.pointer, len);
    return OD_HAL_I2C_OK;
}

int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7, const uint8_t *tx, uint16_t tn,
                          uint8_t *rx, uint16_t rn)
{
    if (od_hal_i2c_write(bus_id, addr7, tx, tn) != OD_HAL_I2C_OK) {
        return OD_HAL_I2C_EIO;
    }
    return od_hal_i2c_read(bus_id, addr7, rx, rn);
}

int od_hal_i2c_probe(uint8_t bus_id, uint8_t addr7)
{
    (void)bus_id;
    return (g_part.present && addr7 == g_part.addr7) ? OD_HAL_I2C_OK : OD_HAL_I2C_ENODEV;
}

/* ------------------------------------------------------------------ the app seam */

static uint8_t  g_msd[11];
static unsigned g_publishes;
static unsigned g_boosts;
static int      g_int_level = 1;
static unsigned g_enable_asserted_at;   /* trace index, to prove it precedes bus traffic */
static bool     g_attach_ok = true;
static unsigned g_attached;
static unsigned g_detached;

void od_touch_app_delay_ms(uint16_t ms) { (void)ms; }
void od_touch_app_delay_us(uint32_t us) { (void)us; }

void od_touch_app_gpio_set_mode_output(uint8_t pin) { trace("out:%u", pin, 0); }
void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up) { (void)pull_up; trace("in:%u", pin, 0); }
void od_touch_app_gpio_write(uint8_t pin, bool level_high) { trace("w:%u:%u", pin, level_high ? 1u : 0u); }
int  od_touch_app_gpio_read(uint8_t pin) { (void)pin; return g_int_level; }

bool od_touch_app_gpio_attach_int(uint8_t idx, uint8_t pin)
{
    (void)idx; (void)pin;
    if (!g_attach_ok) {
        return false;
    }
    g_attached++;
    return true;
}
void od_touch_app_gpio_detach_int(uint8_t pin) { (void)pin; g_detached++; }

bool od_touch_app_bus_prepare(uint8_t bus_id)
{
    trace("bus:%u", bus_id, 0);
    return bus_id == g_bus_ready_id;
}

void od_touch_app_msd_write(uint8_t index, uint8_t value)
{
    if (index < sizeof g_msd) {
        g_msd[index] = value;
    }
}
void od_touch_app_msd_publish(void) { g_publishes++; trace("publish", 0, 0); }
void od_adv_app_boost(void) { g_boosts++; trace("boost", 0, 0); }

/* ------------------------------------------------------------------ fixtures */

static struct od_config g_cfg;

#define BUS_INSTANCE 3u
#define PIN_RST 20u
#define PIN_INT 21u
#define PIN_EN  22u

static void part_reset(uint8_t addr7)
{
    memset(&g_part, 0, sizeof g_part);
    g_part.present = true;
    g_part.addr7 = addr7;
    g_part.answers_high_first = true;
    g_part.regs[0x8140u - GT911_REG_BASE] = '9';
    g_part.regs[0x8141u - GT911_REG_BASE] = '1';
    g_part.regs[0x8142u - GT911_REG_BASE] = '1';
    g_part.regs[0x8143u - GT911_REG_BASE] = 0;
}

static void part_set_contact(uint8_t count, uint8_t track_id, uint16_t x, uint16_t y)
{
    g_part.regs[0x814Eu - GT911_REG_BASE] = (uint8_t)(0x80u | count);
    g_part.regs[0x814Fu - GT911_REG_BASE] = track_id;
    g_part.regs[0x8150u - GT911_REG_BASE] = (uint8_t)(x & 0xFFu);
    g_part.regs[0x8151u - GT911_REG_BASE] = (uint8_t)(x >> 8);
    g_part.regs[0x8152u - GT911_REG_BASE] = (uint8_t)(y & 0xFFu);
    g_part.regs[0x8153u - GT911_REG_BASE] = (uint8_t)(y >> 8);
}

static void setup(uint8_t start_byte, uint8_t flags, uint8_t addr, uint8_t int_pin)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    memset(g_msd, 0xA5, sizeof g_msd);
    g_trace_len = 0;
    g_publishes = 0;
    g_boosts = 0;
    g_attached = 0;
    g_detached = 0;
    g_attach_ok = true;
    g_int_level = 1;
    g_enable_asserted_at = 0;
    g_bus_ready_id = BUS_INSTANCE;

    g_cfg.data_bus_count = 1u;
    g_cfg.data_buses[0].instance_number = BUS_INSTANCE;
    g_cfg.data_buses[0].bus_type = 0x01u;
    g_cfg.data_buses[0].pin_1 = 10u;
    g_cfg.data_buses[0].pin_2 = 11u;

    g_cfg.display_count = 1u;
    g_cfg.displays[0].pixel_width = 320u;
    g_cfg.displays[0].pixel_height = 240u;

    g_cfg.touch_controller_count = 1u;
    g_cfg.touch_controllers[0].touch_ic_type = OD_TOUCH_IC_GT911;
    g_cfg.touch_controllers[0].bus_id = BUS_INSTANCE;
    g_cfg.touch_controllers[0].i2c_addr_7bit = addr;
    g_cfg.touch_controllers[0].rst_pin = PIN_RST;
    g_cfg.touch_controllers[0].int_pin = int_pin;
    g_cfg.touch_controllers[0].enable_pin = PIN_EN;
    g_cfg.touch_controllers[0].display_instance = 0u;
    g_cfg.touch_controllers[0].flags = flags;
    g_cfg.touch_controllers[0].touch_data_start_byte = start_byte;
    g_cfg.touch_controllers[0].poll_interval_ms = 0u;
}

/* ------------------------------------------------------------------ the packing contract */

static void test_msd_packing(void)
{
    CASE("never touched clears the whole 5-byte block");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.regs[0x814Eu - GT911_REG_BASE] = 0x80u;       /* ready, zero contacts */
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_msd[0] == 0u && g_msd[1] == 0u && g_msd[2] == 0u && g_msd[3] == 0u && g_msd[4] == 0u);

    CASE("one contact: count in the low nibble, id in the high, x and y little-endian");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 2u, 0x0123u, 0x00ABu);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_msd[0] == (uint8_t)(1u | (2u << 4)));
    CHECK(g_msd[1] == 0x23u);
    CHECK(g_msd[2] == 0x01u);
    CHECK(g_msd[3] == 0xABu);
    CHECK(g_msd[4] == 0x00u);

    CASE("five contacts still fit the low nibble");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(5u, 0u, 10u, 20u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK((g_msd[0] & 0x0Fu) == 5u);

    CASE("release packs 6 and RETAINS the last coordinates");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 3u, 100u, 200u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    g_part.regs[0x814Eu - GT911_REG_BASE] = 0x80u;       /* ready, released */
    (void)od_touch_gt911_service(&g_cfg, 3000u, 0u, NULL);
    CHECK((g_msd[0] & 0x0Fu) == 6u);
    CHECK((g_msd[0] >> 4) == 3u);
    CHECK(g_msd[1] == 100u && g_msd[2] == 0u);
    CHECK(g_msd[3] == 200u && g_msd[4] == 0u);

    CASE("the block lands at touch_data_start_byte and touches nothing else");
    setup(6u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 0u, 1u, 2u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_msd[6] == 1u && g_msd[7] == 1u && g_msd[9] == 2u);
    CHECK(g_msd[5] == 0xA5u);                            /* the byte below is untouched */

    CASE("a start byte past the 5-byte window is refused, not clamped");
    setup(7u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0u);
    CHECK(g_msd[6] == 0xA5u && g_msd[7] == 0xA5u);       /* nothing written anywhere */
}

/* ------------------------------------------------------------------ the coordinate map */

static void test_coordinate_map(void)
{
    CASE("no flags: raw coordinates, clipped to the panel");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 0u, 999u, 999u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(((uint16_t)g_msd[1] | ((uint16_t)g_msd[2] << 8)) == 319u);
    CHECK(((uint16_t)g_msd[3] | ((uint16_t)g_msd[4] << 8)) == 239u);

    CASE("INVERT_X mirrors against the panel width");
    setup(0u, OD_TOUCH_FLAG_INVERT_X, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 0u, 100u, 50u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(((uint16_t)g_msd[1] | ((uint16_t)g_msd[2] << 8)) == 219u);   /* 320-1-100 */
    CHECK(((uint16_t)g_msd[3] | ((uint16_t)g_msd[4] << 8)) == 50u);

    CASE("SWAP_XY happens BEFORE invert and clip");
    setup(0u, (uint8_t)(OD_TOUCH_FLAG_SWAP_XY | OD_TOUCH_FLAG_INVERT_X), 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 0u, 30u, 100u);      /* swap -> (100,30), invert x -> (219,30) */
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(((uint16_t)g_msd[1] | ((uint16_t)g_msd[2] << 8)) == 219u);
    CHECK(((uint16_t)g_msd[3] | ((uint16_t)g_msd[4] << 8)) == 30u);

    /* THE ONE THAT CATCHES A MAP APPLIED TOO LATE, and it took two goes to write.
     *
     * The obvious probe -- "does the latched release report mapped coordinates?" -- does NOT
     * distinguish them: a driver that caches raw and maps at pack time re-maps the cached raw
     * value to the same pixel, so the bytes agree. What separates them is CHANGE DETECTION.
     * `changed` compares against the cache, so with the map before it two raw coordinates that
     * clip to the same pixel are one sample and publish once; with the map after it they are two
     * samples and publish twice. Same MSD bytes, different traffic.
     */
    CASE("two raw coordinates that CLIP to the same pixel are one sample, not two");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 0u, 900u, 50u);            /* clips to x=319 */
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    g_publishes = 0;
    part_set_contact(1u, 0u, 950u, 50u);            /* also clips to x=319 */
    (void)od_touch_gt911_service(&g_cfg, 3000u, 0u, NULL);
    CHECK(g_publishes == 0u);
    CHECK(((uint16_t)g_msd[1] | ((uint16_t)g_msd[2] << 8)) == 319u);

    CASE("the latched release reports MAPPED coordinates");
    setup(0u, OD_TOUCH_FLAG_INVERT_X, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 0u, 100u, 50u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    g_part.regs[0x814Eu - GT911_REG_BASE] = 0x80u;
    (void)od_touch_gt911_service(&g_cfg, 3000u, 0u, NULL);
    CHECK((g_msd[0] & 0x0Fu) == 6u);
    CHECK(((uint16_t)g_msd[1] | ((uint16_t)g_msd[2] << 8)) == 219u);
}

/* ------------------------------------------------------------------ probe and address */

static void test_byte_order(void)
{
    CASE("the DOCUMENTED order is tried first");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);
    CHECK(trace_has("ptr:BE:8140"));
    CHECK(!trace_has("ptr:LE:8140"));      /* never needed the fallback */

    CASE("a part that answers only the undocumented order still binds, one probe later");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    g_part.answers_high_first = false;
    g_part.answers_low_first = true;
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);
    CHECK(trace_has("ptr:LE:8140"));
    /* The refused attempt put 0x81 on the wire first -- the documented order -- and only the
     * SECOND attempt, with 0x40 first, was answered. */
    CHECK(trace_index("ptr:junk:8140") >= 0);
    CHECK(trace_index("ptr:junk:8140") < trace_index("ptr:LE:8140"));
}

static void test_address_cascade(void)
{
    CASE("a CONFIGURED address that does not answer returns failure, without auto-detecting");
    setup(0u, 0u, 0x14u, 0xFFu);          /* config says 0x14 */
    part_reset(0x5Du);                    /* the part is at 0x5D */
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0u);

    CASE("auto-detect finds 0x5D");
    setup(0u, 0u, 0xFFu, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);

    CASE("auto-detect falls through to 0x14");
    setup(0u, 0u, 0xFFu, 0xFFu);
    part_reset(0x14u);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0x14u);
}

static void test_enable_pin(void)
{
    CASE("enable_pin is driven high BEFORE any bus traffic");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(trace_index("w:22:1") >= 0);
    CHECK(trace_index("bus:3") >= 0);
    CHECK(trace_index("w:22:1") < trace_index("bus:3"));
}

/* ------------------------------------------------------------------ the status byte */

static void test_status_handling(void)
{
    CASE("an over-count sample CLEARS the status, or the part wedges");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.status_clears = 0;
    g_part.regs[0x814Eu - GT911_REG_BASE] = (uint8_t)(0x80u | 9u);   /* 9 > 5: nonsense */
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_part.status_clears == 1u);

    CASE("and having cleared it, the next poll is not stuck on the same branch");
    part_set_contact(1u, 0u, 5u, 6u);
    (void)od_touch_gt911_service(&g_cfg, 3000u, 0u, NULL);
    CHECK((g_msd[0] & 0x0Fu) == 1u);

    CASE("buffer-not-ready must NOT be acknowledged");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.status_clears = 0;
    g_part.regs[0x814Eu - GT911_REG_BASE] = 0x00u;      /* bit 7 clear */
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_part.status_clears == 0u);

    CASE("a consumed sample IS acknowledged");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.status_clears = 0;
    part_set_contact(1u, 0u, 1u, 1u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_part.status_clears == 1u);
}

/* ------------------------------------------------------------------ publication */

static void test_publish_on_change(void)
{
    CASE("a changed sample boosts and publishes, in that order");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_publishes = 0;
    g_boosts = 0;
    g_trace_len = 0;
    part_set_contact(1u, 0u, 40u, 50u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_publishes == 1u);
    CHECK(g_boosts == 1u);
    CHECK(trace_index("boost") >= 0 && trace_index("publish") >= 0);
    CHECK(trace_index("boost") < trace_index("publish"));

    CASE("an identical sample publishes nothing");
    g_publishes = 0;
    g_boosts = 0;
    part_set_contact(1u, 0u, 40u, 50u);
    (void)od_touch_gt911_service(&g_cfg, 3000u, 0u, NULL);
    CHECK(g_publishes == 0u);
    CHECK(g_boosts == 0u);

    CASE("a moved contact publishes again");
    part_set_contact(1u, 0u, 41u, 50u);
    (void)od_touch_gt911_service(&g_cfg, 4000u, 0u, NULL);
    CHECK(g_publishes == 1u);
}

/* ------------------------------------------------------------------ cadence and failure */

static void test_cadence(void)
{
    CASE("poll_interval_ms 0 means 100 ms, not the header's documented 25");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    CHECK(od_touch_gt911_init(&g_cfg, 1000u) == 100u);

    CASE("a configured interval is honoured and gates the read");
    setup(0u, 0u, 0x5Du, 0xFFu);
    g_cfg.touch_controllers[0].poll_interval_ms = 40u;
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.reads = 0;
    CHECK(od_touch_gt911_service(&g_cfg, 1010u, 0u, NULL) == 40u);
    CHECK(g_part.reads == 0u);                       /* too soon */
    (void)od_touch_gt911_service(&g_cfg, 1050u, 0u, NULL);
    CHECK(g_part.reads > 0u);

    CASE("an IRQ edge reads early and is reported as consumed");
    setup(0u, 0u, 0x5Du, PIN_INT);
    g_cfg.touch_controllers[0].poll_interval_ms = 200u;
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    {
        uint8_t consumed = 0xFFu;

        g_part.reads = 0;
        part_set_contact(1u, 0u, 7u, 8u);
        (void)od_touch_gt911_service(&g_cfg, 1005u, 0x01u, &consumed);
        CHECK(consumed == 0x01u);
        CHECK(g_part.reads > 0u);
    }

    CASE("no edge and nothing due consumes nothing");
    {
        uint8_t consumed = 0xFFu;

        (void)od_touch_gt911_service(&g_cfg, 1006u, 0x00u, &consumed);
        CHECK(consumed == 0x00u);
    }
}

static void test_failure_backoff_and_disable(void)
{
    CASE("a failing bus backs off rather than hammering");
    setup(0u, 0u, 0x5Du, 0xFFu);
    g_cfg.touch_controllers[0].poll_interval_ms = 10u;
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.fail_all = true;
    CHECK(od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL) == 100u);

    CASE("five consecutive failures disable the controller");
    (void)od_touch_gt911_service(&g_cfg, 3000u, 0u, NULL);
    (void)od_touch_gt911_service(&g_cfg, 4000u, 0u, NULL);
    (void)od_touch_gt911_service(&g_cfg, 5000u, 0u, NULL);
    CHECK(od_touch_gt911_service(&g_cfg, 6000u, 0u, NULL) == OD_TOUCH_IDLE_MS);
    CHECK(od_touch_gt911_address(0) == 0u);

    CASE("a recovered bus does not resurrect a disabled controller by itself");
    g_part.fail_all = false;
    part_set_contact(1u, 0u, 1u, 1u);
    g_part.reads = 0;
    (void)od_touch_gt911_service(&g_cfg, 7000u, 0u, NULL);
    CHECK(g_part.reads == 0u);
}

/* ------------------------------------------------------------------ bus admission */

static void test_bus_admission(void)
{
    /* A VALID BUS 0 HAS TO EXIST, or this proves nothing. The fixture's only bus is instance 3,
     * so a driver that substituted bus 0 would look up an instance that is not there and refuse
     * for the wrong reason -- passing the test while carrying the defect. Declare instance 0 as a
     * usable bus and make the seam accept it, so substitution SUCCEEDS if the driver does it. */
    CASE("bus_id 0xFF is refused even when a valid bus 0 is there to be substituted");
    setup(0u, 0u, 0x5Du, 0xFFu);
    g_cfg.data_bus_count = 2u;
    g_cfg.data_buses[1].instance_number = 0u;
    g_cfg.data_buses[1].bus_type = 0x01u;
    g_cfg.data_buses[1].pin_1 = 12u;
    g_cfg.data_buses[1].pin_2 = 13u;
    g_cfg.touch_controllers[0].bus_id = 0xFFu;
    g_bus_ready_id = 0u;                            /* the seam would happily serve bus 0 */
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0u);
    CHECK(!trace_has("bus:0"));                     /* it never even asked for it */

    CASE("a declared bus_id with NO data_buses is refused too (the retired default-pin path)");
    setup(0u, 0u, 0x5Du, 0xFFu);
    g_cfg.data_bus_count = 0u;
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0u);

    CASE("the bus is selected by instance_number, not by array index");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(trace_has("bus:3"));
    CHECK(!trace_has("bus:0"));
}

/* ------------------------------------------------------------------ suspend and resume */

static void test_suspend_resume(void)
{
    CASE("suspended, nothing touches the bus");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    od_touch_gt911_suspend();
    g_part.reads = 0;
    part_set_contact(1u, 0u, 1u, 1u);
    CHECK(od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL) == OD_TOUCH_IDLE_MS);
    CHECK(g_part.reads == 0u);

    CASE("resume probes the RETAINED address rather than re-resolving");
    g_trace_len = 0;
    (void)od_touch_gt911_resume(&g_cfg, 3000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);
    CHECK(!trace_has("out:20"));                    /* no reset pin driven: no full re-resolve */

    CASE("suspend nests, and only the last resume acts");
    od_touch_gt911_suspend();
    od_touch_gt911_suspend();
    (void)od_touch_gt911_resume(&g_cfg, 4000u);
    g_part.reads = 0;
    (void)od_touch_gt911_service(&g_cfg, 5000u, 0u, NULL);
    CHECK(g_part.reads == 0u);                      /* still suspended */
    (void)od_touch_gt911_resume(&g_cfg, 6000u);
    (void)od_touch_gt911_service(&g_cfg, 7000u, 0u, NULL);
    CHECK(g_part.reads > 0u);

    CASE("force_resume collapses any depth");
    od_touch_gt911_suspend();
    od_touch_gt911_suspend();
    od_touch_gt911_suspend();
    (void)od_touch_gt911_force_resume(&g_cfg, 8000u);
    g_part.reads = 0;
    part_set_contact(1u, 0u, 2u, 2u);
    (void)od_touch_gt911_service(&g_cfg, 9000u, 0u, NULL);
    CHECK(g_part.reads > 0u);

    CASE("a part that stopped answering gets a FULL re-resolve, not a light resume");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    od_touch_gt911_suspend();
    g_part.regs[0x8140u - GT911_REG_BASE] = 'X';    /* PID no longer matches */
    g_trace_len = 0;
    (void)od_touch_gt911_resume(&g_cfg, 2000u);
    CHECK(trace_has("out:20"));                     /* the reset pin WAS driven */
}

/* ------------------------------------------------------------------ int-pin query */

static void test_int_pin_query(void)
{
    CASE("a configured INT pin is recognised");
    setup(0u, 0u, 0x5Du, PIN_INT);
    CHECK(od_touch_gt911_is_int_pin(&g_cfg, PIN_INT));

    CASE("another pin is not");
    CHECK(!od_touch_gt911_is_int_pin(&g_cfg, PIN_RST));

    CASE("0xFF is never a touch interrupt");
    CHECK(!od_touch_gt911_is_int_pin(&g_cfg, 0xFFu));
}

int main(void)
{
    test_msd_packing();
    test_coordinate_map();
    test_byte_order();
    test_address_cascade();
    test_enable_pin();
    test_status_handling();
    test_publish_on_change();
    test_cadence();
    test_failure_backoff_and_disable();
    test_bus_admission();
    test_suspend_resume();
    test_int_pin_query();

    printf("touch_gt911: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
