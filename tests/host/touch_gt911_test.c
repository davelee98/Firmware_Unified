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

#define BUS_INSTANCE 3u
#define PIN_RST 20u
#define PIN_INT 21u
#define PIN_EN  22u

#define GT911_REG_BASE 0x8140u
#define GT911_REG_SPAN 0x60u

struct fake_part {
    bool     present;
    uint8_t  addr7;            /* the address the part currently ANSWERS on */
    uint8_t  strap_addr7;      /* what the last reset selected, per INT's level at RST's edge */
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
static uint8_t  g_bus_wired_id = 0xFFu;   /* the bus the part is physically ON */

/* THE PART IS ON A BUS, and answers nothing on any other. Without this the fake ignored bus_id
 * entirely, so a driver that prepared the right bus and then transacted on the wrong one passed. */
static bool part_on_bus(uint8_t bus_id)
{
    return bus_id == g_bus_wired_id;
}

/* Pin state, so the reset dance can actually select an address the way silicon does. */
static bool g_pin_int_high;
static bool g_pin_rst_high;
static bool g_saw_rst_rise;
static bool g_int_driven_since_rst_low;

/* Ordered log of what the driver asked the part, so ORDER is assertable and not inferred. */
#define TRACE_MAX 512
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
    if (!part_on_bus(bus_id) || !g_part.present || g_part.fail_all || addr7 != g_part.addr7) {
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

    if (!part_on_bus(bus_id) || !g_part.present || g_part.fail_all || addr7 != g_part.addr7) {
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

/* RECORDED SEPARATELY, because the two framings are two different HAL calls and a driver that
 * used the wrong one would otherwise pass -- the fake would produce identical bytes. This is the
 * repeated-START form (no STOP between the phases). */
int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7, const uint8_t *tx, uint16_t tn,
                          uint8_t *rx, uint16_t rn)
{
    trace("framing:rstart", 0, 0);
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

/* Reconfiguring a pad as an input destroys its interrupt on BOTH stacks -- Zephyr frees the
 * GPIOTE channel, IDF sets intr_type = DISABLE. The fake models that, so a driver that fails to
 * re-attach after waking INT is visible here rather than only on a bench. */
static bool     g_int_trigger_armed;

/* TRACED, because the reset timings are the datasheet's minima and the driver's comment says not
 * to tidy them. Only their ORDER was asserted before; nothing pinned the values. */
void od_touch_app_delay_ms(uint16_t ms) { trace("ms:%u", ms, 0); }
void od_touch_app_delay_us(uint32_t us) { trace("us:%u", (unsigned)us, 0); }

void od_touch_app_gpio_set_mode_output(uint8_t pin) { trace("out:%u", pin, 0); }
void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up)
{
    (void)pull_up;
    trace("in:%u", pin, 0);
    if (pin == PIN_INT) {
        g_int_trigger_armed = false;      /* the reconfigure ate the trigger */
    }
}
/* INT's LEVEL AT RST'S RISING EDGE SELECTS THE ADDRESS, exactly as Rev.10 4.2 describes: high
 * selects 7-bit 0x14, low selects 0x5D. Modelling it is what makes the auto-detect tests real --
 * before this the fake answered on a fixed address and a driver that skipped the reset dance
 * entirely, or drove the pins in the wrong order, passed both cascade cases. */
void od_touch_app_gpio_write(uint8_t pin, bool level_high)
{
    trace("w:%u:%u", pin, level_high ? 1u : 0u);
    if (pin == PIN_INT) {
        g_pin_int_high = level_high;
        g_int_driven_since_rst_low = true;
    }
    if (pin == PIN_RST) {
        if (!level_high) {
            g_int_driven_since_rst_low = false;
        } else if (!g_pin_rst_high) {
            /* Only a host that DROVE INT can select an address. With no INT pin the strap does
             * not move and the part keeps whatever it had -- so a config with rst but no int
             * cannot reach 0x14 by resetting, and auto-detect has to find the part where it is. */
            if (g_int_driven_since_rst_low) {
                g_part.strap_addr7 = g_pin_int_high ? 0x14u : 0x5Du;
                g_part.addr7 = g_part.strap_addr7;
                trace("strap:%02X", g_part.strap_addr7, 0);
            }
            g_saw_rst_rise = true;
        }
        g_pin_rst_high = level_high;
    }
}
/* HONOURS THE PIN. Ignoring it is the same over-forgiving shape that hid a wrong-bus driver
 * before the bus_id fix: a driver reading another controller's INT line would have passed. */
static uint8_t g_int_level_pin = PIN_INT;
int od_touch_app_gpio_read(uint8_t pin)
{
    if (pin != g_int_level_pin) {
        return 1;                     /* not the line under test: idle high */
    }
    return g_int_level;
}

/* MODELS A FINITE, PER-PIN REGISTRY, because that is what both stacks have and it is where the
 * defect lives: Nordic has eight callback slots, and an ISR left on a pin the config moved away
 * from consumes one for the rest of the boot. A fake that only counts attaches cannot see it. */
#define FAKE_IRQ_SLOTS 8u
static uint8_t  g_irq_pins[FAKE_IRQ_SLOTS];   /* 0 = free */

static unsigned irq_slots_used(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < FAKE_IRQ_SLOTS; i++) {
        if (g_irq_pins[i] != 0u) {
            n++;
        }
    }
    return n;
}

static bool irq_pin_attached(uint8_t pin)
{
    unsigned i;

    for (i = 0; i < FAKE_IRQ_SLOTS; i++) {
        if (g_irq_pins[i] == pin) {
            return true;
        }
    }
    return false;
}

bool od_touch_app_gpio_attach_int(uint8_t idx, uint8_t pin)
{
    unsigned i;

    (void)idx;
    if (!g_attach_ok) {
        return false;
    }
    for (i = 0; i < FAKE_IRQ_SLOTS; i++) {
        if (g_irq_pins[i] == 0u) {
            g_irq_pins[i] = pin;
            g_attached++;
            g_int_trigger_armed = true;
            trace("irq+:%u", pin, 0);
            return true;
        }
    }
    return false;                      /* every slot consumed */
}

void od_touch_app_gpio_detach_int(uint8_t pin)
{
    unsigned i;

    for (i = 0; i < FAKE_IRQ_SLOTS; i++) {
        if (g_irq_pins[i] == pin) {
            g_irq_pins[i] = 0u;
            g_detached++;
            trace("irq-:%u", pin, 0);
            return;
        }
    }
}

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
void od_touch_app_bus_invalidate(void) { trace("bus:invalidate", 0, 0); }
void od_adv_app_boost(void) { g_boosts++; trace("boost", 0, 0); }

/* ------------------------------------------------------------------ fixtures */

static struct od_config g_cfg;

static void part_reset(uint8_t addr7)
{
    memset(&g_part, 0, sizeof g_part);
    g_part.present = true;
    g_part.addr7 = addr7;
    g_part.strap_addr7 = addr7;
    g_bus_wired_id = BUS_INSTANCE;
    g_pin_int_high = false;
    g_pin_rst_high = false;
    g_saw_rst_rise = false;
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
    memset(g_irq_pins, 0, sizeof g_irq_pins);
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

    /* CLEAR THE DRIVER'S OWN STATE. s_rt is a file static that outlives one test, and since the
     * retained-runtime path landed a stale `ok` from the previous case makes the next init keep a
     * controller instead of bringing it up -- which silently skips the enable pin, the reset dance
     * and the probe.
     *
     * THE SUSPEND DEPTH NEEDS ITS OWN CALL, and that is not an oversight in init(): it deliberately
     * does NOT clear the counter, because a re-init inside a refresh bracket must not drop an
     * outstanding suspend. The cost is that a test which leaves the machine suspended silently
     * suspends the next one -- which is exactly what happened, and it presented as a held-low line
     * that would not trigger a read. */
    while (od_touch_gt911_force_resume(&g_cfg, 0u) != OD_TOUCH_NO_CHANGE) {
    }
    (void)od_touch_gt911_init(NULL, 0u);
    g_trace_len = 0;
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
    /* THE REMAINDER, not the whole interval. Asking for a full 40 ms again 10 ms in pushes the
     * deadline out every time this controller is polled early for another reason; with two
     * controllers at 99 and 100 ms the slower one drifts to ~198 ms. The first version of this
     * test asserted 40 and so PINNED the defect. */
    CHECK(od_touch_gt911_service(&g_cfg, 1010u, 0u, NULL) == 30u);
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

/* Both of these pin fixes made in review, and both were initially untested -- the mutants that
 * undid them survived a 76-check suite. */
static void test_no_change_and_retry_latency(void)
{
    /* abortToKnownState() force-resumes on EVERY teardown, including ordinary disconnects with
     * nothing suspended. If that reported a delay, a caller installing it would postpone actively
     * polling touch by a second each time. Doing nothing has to be reportable as doing nothing. */
    CASE("an unmatched resume reports NO_CHANGE, not an idle delay");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_force_resume(&g_cfg, 2000u) == OD_TOUCH_NO_CHANGE);
    CHECK(od_touch_gt911_resume(&g_cfg, 2000u) == OD_TOUCH_NO_CHANGE);

    CASE("a nested resume that is still suspended also reports NO_CHANGE");
    od_touch_gt911_suspend();
    od_touch_gt911_suspend();
    CHECK(od_touch_gt911_resume(&g_cfg, 3000u) == OD_TOUCH_NO_CHANGE);
    CHECK(od_touch_gt911_resume(&g_cfg, 3000u) != OD_TOUCH_NO_CHANGE);   /* the last one acts */

    /* Stamping the poll clock for a poll that never happened makes a controller whose bus was
     * briefly unavailable wait its whole interval again instead of retrying next pass. */
    CASE("a failed bus prepare does not consume the polling slot");
    setup(0u, 0u, 0x5Du, 0xFFu);
    g_cfg.touch_controllers[0].poll_interval_ms = 200u;
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    part_set_contact(1u, 0u, 5u, 6u);
    g_bus_ready_id = 0xFEu;                      /* prepare fails */
    (void)od_touch_gt911_service(&g_cfg, 1300u, 0u, NULL);
    CHECK(g_msd[0] == 0xA5u);                    /* nothing read */
    g_bus_ready_id = BUS_INSTANCE;               /* bus is back, well inside the interval */
    (void)od_touch_gt911_service(&g_cfg, 1350u, 0u, NULL);
    CHECK((g_msd[0] & 0x0Fu) == 1u);             /* retried at once, not 200 ms later */
}

/* The donor's post-refresh "kept" path, restored after review found it dropped. Re-running the
 * reset dance on a live part costs ~500 ms per controller and re-selects an address that was
 * already right, so a second init must NOT do it. */
static void test_retained_runtime_reinit(void)
{
    CASE("a second init KEEPS a controller that is already up");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);
    g_trace_len = 0;
    g_saw_rst_rise = false;
    (void)od_touch_gt911_init(&g_cfg, 2000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);
    CHECK(!g_saw_rst_rise);                 /* no reset dance */
    CHECK(trace_has("clear:status"));       /* but the part IS re-acknowledged */

    CASE("if the bus will not come back, it falls through to a full bring-up");
    g_trace_len = 0;
    g_saw_rst_rise = false;
    g_bus_ready_id = 0xFEu;                 /* prepare fails for every bus */
    (void)od_touch_gt911_init(&g_cfg, 3000u);
    CHECK(od_touch_gt911_address(0) == 0u);
    CHECK(trace_has("bus:3"));              /* it tried */
}

/* Both found by review of the Nordic half, and neither was covered by the 88 checks that
 * preceded them -- the mutants undoing them passed. */
static void test_reestablish_and_int_rearm(void)
{
    /* Nordic's post-refresh hook is UNPAIRED: one call site, no suspend anywhere. Gating recovery
     * on the suspend count made it a silent no-op there, so the controller was never re-probed
     * after the panel took the bus. */
    CASE("reestablish works with nothing suspended");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_trace_len = 0;
    CHECK(od_touch_gt911_reestablish(&g_cfg, 2000u) != OD_TOUCH_NO_CHANGE);
    CHECK(trace_has("bus:invalidate"));
    CHECK(trace_has("clear:status"));            /* the part WAS re-acknowledged */
    CHECK(od_touch_gt911_address(0) == 0x5Du);

    CASE("reestablish falls back to a full reset when the part stops answering");
    g_part.regs[0x8140u - GT911_REG_BASE] = 'X';
    g_saw_rst_rise = false;
    (void)od_touch_gt911_reestablish(&g_cfg, 3000u);
    CHECK(g_saw_rst_rise);

    /* Waking INT reconfigures the pad, which destroys the interrupt on both stacks. Re-attaching
     * only `if (!int_attached)` left the flag saying attached while the hardware trigger was
     * gone: edges stopped advancing service and only the timed poll survived. */
    CASE("the interrupt is RE-ARMED after a resume, not assumed still attached");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(g_int_trigger_armed);
    (void)od_touch_gt911_reestablish(&g_cfg, 2000u);
    CHECK(g_int_trigger_armed);

    CASE("and an edge still reaches the machine afterwards");
    {
        uint8_t consumed = 0u;

        part_set_contact(1u, 0u, 9u, 9u);
        (void)od_touch_gt911_service(&g_cfg, 2005u, 0x01u, &consumed);
        CHECK(consumed == 0x01u);
    }
}

/* THE SUBJECT OF 6bfa613 WAS UNPINNED. Every fixture had one controller, so the remainder-vs-
 * interval contract, the min() aggregation across controllers and the bit-to-index mapping of the
 * IRQ mask were all untested -- a driver that returned the whole interval again, or that indexed
 * the runtime by one number and the mask by another, passed all 88 checks. */
static void setup_two(uint8_t interval_a, uint8_t interval_b)
{
    setup(0u, 0u, 0x5Du, 0xFFu);
    g_cfg.touch_controller_count = 2u;
    g_cfg.touch_controllers[1] = g_cfg.touch_controllers[0];
    g_cfg.touch_controllers[0].poll_interval_ms = interval_a;
    g_cfg.touch_controllers[1].poll_interval_ms = interval_b;
    g_cfg.touch_controllers[1].touch_data_start_byte = 5u;
}

static void test_multi_controller(void)
{
    CASE("the machine asks for the SOONEST deadline across controllers");
    setup_two(40u, 100u);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_service(&g_cfg, 1000u, 0u, NULL) == 40u);

    /* The concrete drift 6bfa613 describes: at 99/100 ms, returning the whole interval instead of
     * the remainder pushes the slower controller out on every pass the faster one causes. */
    CASE("a controller polled early for its neighbour keeps its own deadline");
    setup_two(99u, 100u);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    (void)od_touch_gt911_service(&g_cfg, 1099u, 0u, NULL);      /* A is due, B is not */
    CHECK(od_touch_gt911_service(&g_cfg, 1099u, 0u, NULL) == 1u); /* B wants 1 ms, not 100 */

    /* Both controllers are scripted at one address on one bus, so they ARE one fake part: the
     * first to poll acknowledges the status and the second correctly sees not-ready. Give each
     * its own pass with the sample re-armed, which is what two real parts would present. */
    /* Only the SECOND controller is due, so it polls alone. Both are scripted at one address on
     * one bus and are therefore one fake part -- whichever polls first acknowledges the status and
     * the other correctly sees not-ready, so a shared-sample assertion would be testing the
     * fixture. Isolating controller 1 tests what actually matters: that it writes its OWN block
     * and touches nobody else's. */
    CASE("a controller writes its own block at its own start byte, and no other");
    setup_two(255u, 10u);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    memset(g_msd, 0xA5, sizeof g_msd);
    part_set_contact(1u, 0u, 0x0102u, 0x0304u);
    (void)od_touch_gt911_service(&g_cfg, 1200u, 0u, NULL);      /* A not due, B due */
    CHECK(g_msd[5] == (uint8_t)1u && g_msd[6] == 0x02u && g_msd[7] == 0x01u);
    CHECK(g_msd[0] == 0xA5u && g_msd[4] == 0xA5u);              /* A's block untouched */
    CHECK(g_msd[10] == 0xA5u);                                  /* nothing past B's block */

    CASE("an edge for controller 1 is reported against BIT 1, not bit 0");
    setup_two(200u, 200u);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    {
        uint8_t consumed = 0u;

        part_set_contact(1u, 0u, 1u, 1u);
        (void)od_touch_gt911_service(&g_cfg, 1005u, 0x02u, &consumed);
        CHECK(consumed == 0x02u);
    }
}

/* A set bit that the machine will never act on has to be DISCARDED. Both wrappers gate their whole
 * early return on `mask == 0`, so one stuck bit means the service walk runs on every loop pass for
 * ever and the deadline is never consulted again. */
static void test_stuck_irq_bits(void)
{
    uint8_t consumed;

    CASE("an edge for a controller that is not configured is discarded");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    consumed = 0u;
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0x08u, &consumed);   /* index 3: absent */
    CHECK((consumed & 0x08u) != 0u);

    CASE("an edge for a DISABLED controller is discarded");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.fail_all = true;
    {
        uint32_t t;

        for (t = 2000u; t <= 7000u; t += 1000u) {
            (void)od_touch_gt911_service(&g_cfg, t, 0u, NULL);
        }
    }
    CHECK(od_touch_gt911_address(0) == 0u);                          /* disabled */
    consumed = 0u;
    (void)od_touch_gt911_service(&g_cfg, 8000u, 0x01u, &consumed);
    CHECK((consumed & 0x01u) != 0u);

    CASE("edges raised while suspended are discarded, not held for the whole refresh");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    od_touch_gt911_suspend();
    consumed = 0u;
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0x01u, &consumed);
    CHECK(consumed == 0x01u);
}

/* The per-poll bound the authority checks on EVERY poll, not only at bring-up: a config write can
 * move touch_data_start_byte while a controller is up, and the retained branch keeps it. */
static void test_msd_bound_on_every_poll(void)
{
    CASE("a controller kept across init cannot scribble past the dynamic block");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);
    g_cfg.touch_controllers[0].touch_data_start_byte = 8u;   /* config moved under a live one */
    (void)od_touch_gt911_init(&g_cfg, 2000u);
    memset(g_msd, 0xA5, sizeof g_msd);
    part_set_contact(1u, 0u, 1u, 2u);
    (void)od_touch_gt911_service(&g_cfg, 3000u, 0u, NULL);
    CHECK(g_msd[8] == 0xA5u && g_msd[9] == 0xA5u && g_msd[10] == 0xA5u);

    /* THE ABOVE ONLY PROVES THE RETAINED-BRANCH GUARD. Removing the bound inside touch_pack_msd()
     * still passed it, because bring-up refuses an out-of-range start and the retained branch now
     * declines to keep one -- so the controller never comes up and nothing is written either way.
     *
     * The pack-time bound defends a different path, and it is the one the authority checks on
     * EVERY poll: `cfg` is passed to service() afresh each call, so a config that changes while a
     * controller is up reaches the packer without any init in between. */
    CASE("a start byte that moves out of range with no re-init is still refused at pack time");
    setup(0u, 0u, 0x5Du, 0xFFu);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(od_touch_gt911_address(0) == 0x5Du);
    memset(g_msd, 0xA5, sizeof g_msd);
    g_cfg.touch_controllers[0].touch_data_start_byte = 8u;   /* no init follows */
    part_set_contact(1u, 0u, 1u, 2u);
    (void)od_touch_gt911_service(&g_cfg, 2000u, 0u, NULL);
    CHECK(g_msd[8] == 0xA5u && g_msd[9] == 0xA5u && g_msd[10] == 0xA5u);
}

/* The held-low line: a report waiting whose edge was missed. Never exercised before -- g_int_level
 * was set to 1 by every fixture and never cleared. */
static void test_held_low_line(void)
{
    CASE("a held-low INT line triggers a read with no edge and nothing due");
    setup(0u, 0u, 0x5Du, PIN_INT);
    g_cfg.touch_controllers[0].poll_interval_ms = 200u;
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_part.reads = 0;
    part_set_contact(1u, 0u, 3u, 4u);
    g_int_level = 0;                                  /* asserted */
    (void)od_touch_gt911_service(&g_cfg, 1005u, 0u, NULL);
    CHECK(g_part.reads > 0u);
    CHECK((g_msd[0] & 0x0Fu) == 1u);
}

/* A config write can move the interrupt pin under a live controller, and nothing re-initialises
 * touch on reload. Recording only THAT an interrupt was attached leaves the old pin's ISR
 * installed for the rest of the boot. */
static void test_int_pin_moves(void)
{
    CASE("moving the INT pin detaches the old one before attaching the new");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(irq_pin_attached(PIN_INT));
    CHECK(irq_slots_used() == 1u);

    g_cfg.touch_controllers[0].int_pin = (uint8_t)(PIN_INT + 1u);
    (void)od_touch_gt911_reestablish(&g_cfg, 2000u);
    CHECK(irq_pin_attached((uint8_t)(PIN_INT + 1u)));
    CHECK(!irq_pin_attached(PIN_INT));                  /* the old ISR is gone */
    CHECK(irq_slots_used() == 1u);                      /* and its slot came back */

    CASE("repeated moves do not exhaust the registry");
    {
        uint8_t p;

        for (p = 2u; p < 12u; p++) {
            g_cfg.touch_controllers[0].int_pin = (uint8_t)(PIN_INT + p);
            (void)od_touch_gt911_reestablish(&g_cfg, 2000u + p);
        }
        CHECK(irq_slots_used() == 1u);
        CHECK(irq_pin_attached((uint8_t)(PIN_INT + 11u)));
    }

    CASE("a re-init detaches by RECORD, so a moved pin is not orphaned");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    g_cfg.touch_controllers[0].int_pin = (uint8_t)(PIN_INT + 1u);
    (void)od_touch_gt911_init(&g_cfg, 2000u);
    CHECK(!irq_pin_attached(PIN_INT));
    CHECK(irq_slots_used() <= 1u);

    /* THE CASE THE INIT-LOOP CHANGE ACTUALLY DEFENDS. When the pin merely moves, the attach-time
     * detach cleans up and this passes either way; when the controller LEAVES the config there is
     * no later attach to clean up after it, and detaching by the new config finds nothing. */
    CASE("a controller removed from the config does not orphan its ISR");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(irq_slots_used() == 1u);
    g_cfg.touch_controller_count = 0u;
    (void)od_touch_gt911_init(&g_cfg, 2000u);
    CHECK(irq_slots_used() == 0u);

    CASE("disabling a controller frees its slot");
    setup(0u, 0u, 0x5Du, PIN_INT);
    part_reset(0x5Du);
    (void)od_touch_gt911_init(&g_cfg, 1000u);
    CHECK(irq_slots_used() == 1u);
    g_part.fail_all = true;
    {
        uint32_t t;

        for (t = 2000u; t <= 8000u; t += 1000u) {
            (void)od_touch_gt911_service(&g_cfg, t, 0u, NULL);
        }
    }
    CHECK(od_touch_gt911_address(0) == 0u);
    CHECK(irq_slots_used() == 0u);
}

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
    test_no_change_and_retry_latency();
    test_retained_runtime_reinit();
    test_reestablish_and_int_rearm();
    test_multi_controller();
    test_stuck_irq_bits();
    test_msd_bound_on_every_poll();
    test_held_low_line();
    test_int_pin_moves();
    test_int_pin_query();

    printf("touch_gt911: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
