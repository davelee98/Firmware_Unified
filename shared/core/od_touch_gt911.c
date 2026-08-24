/* Shared GT911 driver. See od_touch_gt911.h. */

#include "od_touch_gt911.h"

#include "od_hal_i2c.h"
#include "od_log.h"
#include "od_touch_app.h"

#define GT911_REG_PID              0x8140u
#define GT911_REG_STATUS           0x814Eu
/* First contact: track id at 0x814F, X at 0x8150 -- COORD_ADDR + 1. */
#define GT911_REG_POINT1           0x814Fu

#define GT911_STATUS_BUFFER_READY  0x80u
#define GT911_MAX_CONTACTS         5u
#define GT911_I2C_RETRIES          3u
#define GT911_I2C_RETRY_DELAY_US   500u
#define GT911_PRE_RESET_DELAY_MS   300u
#define GT911_POST_RESET_SETTLE_MS 200u
#define TOUCH_I2C_FAIL_DISABLE_THRESHOLD 5u

#define OD_MSD_DYNAMIC_LEN         11u
/* A contact occupies five bytes: [count|id][x lo][x hi][y lo][y hi]. */
#define GT911_MSD_WIDTH            5u

struct gt911_runtime {
    uint8_t  addr7;
    uint8_t  bus_id;
    uint8_t  ok;
    uint8_t  reg_high_first;   /* 0: register low byte first (common); 1: high byte first */
    uint16_t last_x;
    uint16_t last_y;
    uint8_t  last_count;
    uint8_t  last_id;
    uint8_t  touch_latched;
    uint8_t  i2c_fail_streak;
    uint8_t  disabled;
};

static struct gt911_runtime s_rt[OD_TOUCH_MAX_CONTROLLERS];

/* ------------------------------------------------------------------ register access ------ */

/* Clones disagree on the order of the two register-address bytes, which is what reg_high_first
 * selects. The probe settles it per controller. */
static uint8_t gt911_reg_bytes(uint8_t *out, uint16_t reg, bool reg_high_first)
{
    if (reg_high_first) {
        out[0] = (uint8_t)(reg >> 8);
        out[1] = (uint8_t)(reg & 0xFFu);
    } else {
        out[0] = (uint8_t)(reg & 0xFFu);
        out[1] = (uint8_t)(reg >> 8);
    }
    return 2u;
}

/* The longest payload this driver writes is one status byte. */
#define GT911_MAX_WRITE_PAYLOAD 8u

static bool gt911_write_reg(uint8_t bus_id, uint8_t addr7, uint16_t reg,
                            const uint8_t *buf, uint8_t len, bool reg_high_first)
{
    uint8_t tx[2u + GT911_MAX_WRITE_PAYLOAD];
    uint8_t n;

    if (len > GT911_MAX_WRITE_PAYLOAD) {
        return false;
    }
    n = gt911_reg_bytes(tx, reg, reg_high_first);
    for (uint8_t i = 0; i < len; i++) {
        tx[n + i] = buf[i];
    }
    for (uint8_t attempt = 0; attempt < GT911_I2C_RETRIES; attempt++) {
        if (od_hal_i2c_write(bus_id, addr7, tx, (uint16_t)(n + len)) == OD_HAL_I2C_OK) {
            return true;
        }
        od_touch_app_delay_us(GT911_I2C_RETRY_DELAY_US);
    }
    return false;
}

/* One attempt at one framing.
 *
 * repeated_start selects which: write_read is a single transaction with a repeated START, while
 * write() then read() puts a STOP between. Real clones need one or the other, and a part that
 * wants the repeated START answers a STOP-then-START read as if unaddressed -- plausible bytes,
 * not an error -- so the two must stay separately expressible. */
static bool gt911_read_reg_once(uint8_t bus_id, uint8_t addr7, uint16_t reg,
                                uint8_t *buf, uint8_t len, bool reg_high_first,
                                bool repeated_start)
{
    uint8_t tx[2];
    uint8_t n = gt911_reg_bytes(tx, reg, reg_high_first);

    if (repeated_start) {
        return od_hal_i2c_write_read(bus_id, addr7, tx, n, buf, len) == OD_HAL_I2C_OK;
    }
    if (od_hal_i2c_write(bus_id, addr7, tx, n) != OD_HAL_I2C_OK) {
        return false;
    }
    return od_hal_i2c_read(bus_id, addr7, buf, len) == OD_HAL_I2C_OK;
}

static bool gt911_read_reg(uint8_t bus_id, uint8_t addr7, uint16_t reg,
                           uint8_t *buf, uint8_t len, bool reg_high_first)
{
    for (uint8_t attempt = 0; attempt < GT911_I2C_RETRIES; attempt++) {
        if (gt911_read_reg_once(bus_id, addr7, reg, buf, len, reg_high_first, true)) {
            return true;
        }
        od_touch_app_delay_us(GT911_I2C_RETRY_DELAY_US);
        if (gt911_read_reg_once(bus_id, addr7, reg, buf, len, reg_high_first, false)) {
            return true;
        }
        od_touch_app_delay_us(GT911_I2C_RETRY_DELAY_US);
    }
    return false;
}

/* Q7 RULING: the status is cleared after a sample is consumed AND on the over-count branch.
 * GT911 holds 0x814E until the host writes 0, so skipping an out-of-range count without clearing
 * leaves buffer-ready set for ever -- the next poll reads the same byte and takes the same
 * branch, and touch reports nothing until a resume, reinit or the next EPD refresh. The
 * authority does not clear there; Nordic does, and Nordic is right (FOLLOWUPS 17). */
static void gt911_clear_status(uint8_t bus_id, uint8_t addr7, bool reg_high_first)
{
    uint8_t z = 0;

    (void)gt911_write_reg(bus_id, addr7, GT911_REG_STATUS, &z, 1u, reg_high_first);
}

/* ------------------------------------------------------------------ probe and reset ------ */

static bool gt911_product_id_match(const uint8_t *id)
{
    return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

/* Low-byte-first is the common order, so it is tried first; a clone answering the other way
 * settles reg_high_first for every later access to that controller. */
static bool gt911_probe_product(uint8_t bus_id, uint8_t addr7, uint8_t *reg_high_first)
{
    uint8_t id[4];

    if (gt911_read_reg(bus_id, addr7, GT911_REG_PID, id, 4u, false) &&
        gt911_product_id_match(id)) {
        *reg_high_first = 0u;
        return true;
    }
    if (gt911_read_reg(bus_id, addr7, GT911_REG_PID, id, 4u, true) &&
        gt911_product_id_match(id)) {
        *reg_high_first = 1u;
        return true;
    }
    return false;
}

/* ORDER IS THE CONTRACT. Both pads become outputs BEFORE either is driven, and INT's level at
 * RST's rising edge is what selects the controller's I2C address -- 0x5D when INT is low, 0x14
 * when high. Collapsing a set-mode and a write into one call reorders that and changes which
 * address the part answers on. */
static void gt911_hw_reset(const struct TouchController *t, bool int_low_for_addr_5d)
{
    if (t->rst_pin == 0xFFu) {
        return;
    }
    if (t->int_pin == 0xFFu) {
        od_touch_app_gpio_set_mode_output(t->rst_pin);
        od_touch_app_gpio_write(t->rst_pin, false);
        od_touch_app_delay_ms(10u);
        od_touch_app_gpio_write(t->rst_pin, true);
        od_touch_app_delay_ms(60u);
        od_touch_app_gpio_set_mode_output(t->rst_pin);
        od_touch_app_gpio_write(t->rst_pin, true);
        return;
    }
    od_touch_app_delay_ms(1u);
    od_touch_app_gpio_set_mode_output(t->int_pin);
    od_touch_app_gpio_set_mode_output(t->rst_pin);
    od_touch_app_gpio_write(t->int_pin, false);
    od_touch_app_gpio_write(t->rst_pin, false);
    od_touch_app_delay_ms(11u);
    od_touch_app_gpio_write(t->int_pin, !int_low_for_addr_5d);
    od_touch_app_delay_us(110u);
    od_touch_app_gpio_set_mode_output(t->rst_pin);
    od_touch_app_gpio_write(t->rst_pin, true);
    od_touch_app_delay_ms(6u);
    od_touch_app_gpio_write(t->int_pin, false);
    od_touch_app_delay_ms(51u);
    od_touch_app_gpio_set_mode_output(t->rst_pin);
    od_touch_app_gpio_write(t->rst_pin, true);
    od_touch_app_gpio_config_input(t->int_pin, true);
}

/* Resolve the address the controller actually answers on, resetting to select it where a reset
 * pin exists. Returns 0 when nothing answers. */
static uint8_t gt911_resolve_address(const struct TouchController *t, struct gt911_runtime *rt)
{
    const uint8_t a5d = 0x5Du;
    const uint8_t a14 = 0x14u;
    const uint8_t want = t->i2c_addr_7bit;

    if (want != 0u && want != 0xFFu) {
        if (t->rst_pin != 0xFFu) {
            od_touch_app_delay_ms(GT911_PRE_RESET_DELAY_MS);
            gt911_hw_reset(t, want == a5d);
            od_touch_app_delay_ms(GT911_POST_RESET_SETTLE_MS);
        } else {
            od_touch_app_delay_ms(10u);
        }
        if (gt911_probe_product(rt->bus_id, want, &rt->reg_high_first)) {
            return want;
        }
    }
    if (t->rst_pin != 0xFFu) {
        od_touch_app_delay_ms(GT911_PRE_RESET_DELAY_MS);
        gt911_hw_reset(t, true);
        od_touch_app_delay_ms(GT911_POST_RESET_SETTLE_MS);
    }
    if (gt911_probe_product(rt->bus_id, a5d, &rt->reg_high_first)) {
        return a5d;
    }
    if (gt911_probe_product(rt->bus_id, a14, &rt->reg_high_first)) {
        return a14;
    }
    if (t->rst_pin != 0xFFu) {
        od_touch_app_delay_ms(GT911_PRE_RESET_DELAY_MS);
        gt911_hw_reset(t, false);
        od_touch_app_delay_ms(GT911_POST_RESET_SETTLE_MS);
        if (gt911_probe_product(rt->bus_id, a14, &rt->reg_high_first)) {
            return a14;
        }
        if (gt911_probe_product(rt->bus_id, a5d, &rt->reg_high_first)) {
            return a5d;
        }
    }
    return 0u;
}

/* ------------------------------------------------------------------ config ---------------- */

/* 0xFF means no bus was assigned. Refused, never resolved to bus 0 -- probing an unassigned
 * controller on another device's pins is how an address collision returns plausible-but-wrong
 * contacts (DIVERGENCE_MATRIX 13). */
static bool touch_usable(const struct od_config *cfg, const struct TouchController *t)
{
    const struct DataBus *bus;

    if (t->touch_ic_type != OD_TOUCH_IC_GT911 || t->bus_id == 0xFFu) {
        return false;
    }
    if (t->touch_data_start_byte > 6u) {
        return false;   /* five bytes must fit the 11-byte block */
    }
    bus = od_config_data_bus(cfg, t->bus_id);
    return bus != NULL && bus->bus_type == 0x01u &&
           bus->pin_1 != 0xFFu && bus->pin_2 != 0xFFu;
}

bool od_touch_gt911_is_int_pin(const struct od_config *cfg, uint8_t pin)
{
    if (pin == 0xFFu || cfg == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < cfg->touch_controller_count && i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        const struct TouchController *t = &cfg->touch_controllers[i];

        if (t->touch_ic_type == OD_TOUCH_IC_GT911 && t->int_pin == pin) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ init and poll --------- */

void od_touch_gt911_init(const struct od_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    for (uint8_t i = 0; i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        s_rt[i].ok = 0u;
        s_rt[i].disabled = 0u;
        s_rt[i].addr7 = 0u;
        s_rt[i].i2c_fail_streak = 0u;
        s_rt[i].touch_latched = 0u;
    }
    for (uint8_t i = 0; i < cfg->touch_controller_count && i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        const struct TouchController *t = &cfg->touch_controllers[i];
        struct gt911_runtime *rt = &s_rt[i];
        uint8_t addr;

        if (t->touch_ic_type == OD_TOUCH_IC_NONE) {
            continue;
        }
        if (!touch_usable(cfg, t)) {
            od_log_info("touch[%u]: not usable, ic=%u bus=%u", (unsigned)i,
                        (unsigned)t->touch_ic_type, (unsigned)t->bus_id);
            continue;
        }
        rt->bus_id = t->bus_id;
        addr = gt911_resolve_address(t, rt);
        if (addr == 0u) {
            od_log_info("touch[%u]: GT911 not found on bus %u", (unsigned)i,
                        (unsigned)t->bus_id);
            continue;
        }
        rt->addr7 = addr;
        rt->ok = 1u;
        gt911_clear_status(rt->bus_id, rt->addr7, rt->reg_high_first != 0u);
        od_log_info("touch[%u]: GT911 at 0x%02X bus %u order %u", (unsigned)i,
                    (unsigned)addr, (unsigned)t->bus_id, (unsigned)rt->reg_high_first);
    }
}

void od_touch_gt911_resume(const struct od_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    for (uint8_t i = 0; i < cfg->touch_controller_count && i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        struct gt911_runtime *rt = &s_rt[i];

        if (rt->ok == 0u || rt->disabled != 0u) {
            continue;
        }
        /* No reset: the controller kept its address across the panel's use of the bus. Clearing
         * the status discards whatever accumulated while nobody was reading. */
        rt->i2c_fail_streak = 0u;
        rt->touch_latched = 0u;
        gt911_clear_status(rt->bus_id, rt->addr7, rt->reg_high_first != 0u);
    }
}

static void gt911_write_msd(uint8_t start, uint8_t n, uint8_t tid, uint16_t x, uint16_t y)
{
    od_touch_app_msd_write(start, (uint8_t)((n & 0x0Fu) | ((tid & 0x0Fu) << 4)));
    od_touch_app_msd_write((uint8_t)(start + 1u), (uint8_t)(x & 0xFFu));
    od_touch_app_msd_write((uint8_t)(start + 2u), (uint8_t)(x >> 8));
    od_touch_app_msd_write((uint8_t)(start + 3u), (uint8_t)(y & 0xFFu));
    od_touch_app_msd_write((uint8_t)(start + 4u), (uint8_t)(y >> 8));
}

void od_touch_gt911_poll(const struct od_config *cfg, uint32_t now_ms)
{
    (void)now_ms;

    if (cfg == NULL) {
        return;
    }
    for (uint8_t i = 0; i < cfg->touch_controller_count && i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        const struct TouchController *t = &cfg->touch_controllers[i];
        struct gt911_runtime *rt = &s_rt[i];
        const bool rh = rt->reg_high_first != 0u;
        uint8_t start = t->touch_data_start_byte;
        uint8_t st = 0;
        uint8_t n;
        uint16_t x;
        uint16_t y;
        uint8_t tid;

        if (rt->ok == 0u || rt->disabled != 0u) {
            continue;
        }
        if (!gt911_read_reg(rt->bus_id, rt->addr7, GT911_REG_STATUS, &st, 1u, rh)) {
            if (rt->i2c_fail_streak < 255u) {
                rt->i2c_fail_streak++;
            }
            if (rt->i2c_fail_streak >= TOUCH_I2C_FAIL_DISABLE_THRESHOLD) {
                rt->disabled = 1u;
                rt->ok = 0u;
                od_log_info("touch[%u]: disabled after repeated I2C failures", (unsigned)i);
            }
            continue;
        }
        rt->i2c_fail_streak = 0u;

        if ((st & GT911_STATUS_BUFFER_READY) == 0u) {
            continue;
        }
        n = (uint8_t)(st & 0x0Fu);
        if (n > GT911_MAX_CONTACTS) {
            /* Q7: clear, or buffer-ready stays set and this branch repeats for ever. */
            gt911_clear_status(rt->bus_id, rt->addr7, rh);
            continue;
        }

        x = rt->last_x;
        y = rt->last_y;
        tid = rt->last_id;
        if (n > 0u) {
            uint8_t p[8];

            if (!gt911_read_reg(rt->bus_id, rt->addr7, GT911_REG_POINT1, p, 8u, rh)) {
                if (rt->i2c_fail_streak < 255u) {
                    rt->i2c_fail_streak++;
                }
                if (rt->i2c_fail_streak >= TOUCH_I2C_FAIL_DISABLE_THRESHOLD) {
                    rt->disabled = 1u;
                    rt->ok = 0u;
                }
                continue;
            }
            tid = p[0];
            x = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
            y = (uint16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
            rt->touch_latched = 1u;
        }
        gt911_clear_status(rt->bus_id, rt->addr7, rh);

        rt->last_count = n;
        rt->last_x = x;
        rt->last_y = y;
        rt->last_id = tid;

        if ((uint16_t)start + GT911_MSD_WIDTH > OD_MSD_DYNAMIC_LEN) {
            continue;
        }
        if (n == 0u && rt->touch_latched == 0u) {
            /* Never touched since boot: the block stays cleared rather than reporting a
             * release at (0,0). */
            for (uint8_t b = 0; b < GT911_MSD_WIDTH; b++) {
                od_touch_app_msd_write((uint8_t)(start + b), 0u);
            }
        } else if (n == 0u) {
            /* Released: count 6, and the LAST coordinates, so a host can see where it ended. */
            gt911_write_msd(start, 6u, tid, x, y);
        } else {
            gt911_write_msd(start, n, tid, x, y);
        }
    }
}
