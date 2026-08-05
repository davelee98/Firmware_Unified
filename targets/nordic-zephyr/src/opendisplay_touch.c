#include "opendisplay_touch.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "opendisplay_structs.h"
#include "nrf54_gpio.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

/*
 * GT911 capacitive touch driver for the nRF54 port. Ported from
 * OpenDisplay-Firmware src/touch_input.cpp (the Arduino reference) with the
 * transport swapped from Arduino Wire to a small software (bit-banged) I2C
 * master built on the nrf54_gpio open-drain primitives, because the touch
 * bus pins arrive as runtime config bytes (compact (port<<4)|pin encoding)
 * rather than devicetree-fixed I2C controller pins.
 *
 * The MSD dynamic-byte encoding is kept byte-identical to the reference so
 * Home Assistant / clients decode touch state the same way on both firmwares.
 * INT-driven wakeups are not used here: like the button/led modules this is a
 * cooperatively polled module driven from the main loop. The INT pin is still
 * driven during reset (it selects the I2C address) and reserved from the button
 * subsystem.
 */

/* GT911 register map (16-bit register addresses). */
#define GT911_REG_PID 0x8140u
#define GT911_REG_STATUS 0x814Eu
#define GT911_REG_POINT1 0x814Fu /* track id @0x814F, X @0x8150 (COORD_ADDR+1) */

#define GT911_POST_RESET_SETTLE_MS 200u
#define GT911_PRE_RESET_DELAY_MS 300u
#define GT911_STATUS_BUFFER_READY 0x80u
#define GT911_MAX_CONTACTS 5u
#define GT911_I2C_RETRIES 3u

#define TOUCH_PROCESS_MIN_INTERVAL_MS 100u
#define TOUCH_I2C_FAIL_DISABLE_THRESHOLD 5u

/* Software-I2C half-bit period. GT911 tolerates a slow clock; the reconfigure
 * overhead of the open-drain emulation already keeps this well under 100 kHz. */
#define I2C_HALF_BIT_US 5u
#define I2C_STRETCH_TIMEOUT_US 1000u

struct TouchBus {
  uint8_t scl; /* data_bus pin_1 */
  uint8_t sda; /* data_bus pin_2 */
};

struct TouchRuntime {
  uint8_t addr7;
  uint8_t ok;
  uint8_t reg_high_first; /* 0: reg low byte first (common); 1: high byte first */
  uint16_t last_x;
  uint16_t last_y;
  uint8_t last_count;
  uint8_t last_id;
  uint32_t last_poll_ms;
  uint8_t touch_latched;
  uint8_t i2c_fail_streak;
  uint8_t disabled;
  struct TouchBus bus;
};

static struct TouchRuntime s_touch_rt[4];
static uint32_t s_last_process_ms;
static bool s_any_initialized;

/* ---- open-drain bit-bang I2C primitives over nrf54_gpio ---- */

static inline void i2c_delay(void)
{
  k_busy_wait(I2C_HALF_BIT_US);
}

static inline void line_release(uint8_t pin)
{
  /* Let the (internal + any external) pull-up drive the line high. */
  nrf54_gpio_configure_input(pin, true, false);
}

static inline void line_low(uint8_t pin)
{
  nrf54_gpio_configure_output(pin, false);
}

static bool scl_release_wait(const struct TouchBus *b)
{
  line_release(b->scl);
  for (uint32_t i = 0; i < I2C_STRETCH_TIMEOUT_US; i++) {
    if (nrf54_gpio_read(b->scl) != 0) {
      return true;
    }
    k_busy_wait(1);
  }
  return nrf54_gpio_read(b->scl) != 0;
}

static void i2c_start(const struct TouchBus *b)
{
  line_release(b->sda);
  (void)scl_release_wait(b);
  i2c_delay();
  line_low(b->sda);
  i2c_delay();
  line_low(b->scl);
  i2c_delay();
}

static void i2c_stop(const struct TouchBus *b)
{
  line_low(b->sda);
  i2c_delay();
  (void)scl_release_wait(b);
  i2c_delay();
  line_release(b->sda);
  i2c_delay();
}

static bool i2c_write_bit(const struct TouchBus *b, uint8_t bit)
{
  if (bit) {
    line_release(b->sda);
  } else {
    line_low(b->sda);
  }
  i2c_delay();
  if (!scl_release_wait(b)) {
    return false;
  }
  i2c_delay();
  line_low(b->scl);
  i2c_delay();
  return true;
}

static uint8_t i2c_read_bit(const struct TouchBus *b)
{
  uint8_t v;

  line_release(b->sda);
  i2c_delay();
  (void)scl_release_wait(b);
  i2c_delay();
  v = (nrf54_gpio_read(b->sda) != 0) ? 1u : 0u;
  line_low(b->scl);
  i2c_delay();
  return v;
}

/* Returns true on ACK (slave pulled SDA low). */
static bool i2c_write_byte(const struct TouchBus *b, uint8_t val)
{
  for (uint8_t i = 0; i < 8u; i++) {
    if (!i2c_write_bit(b, (uint8_t)((val & 0x80u) ? 1u : 0u))) {
      return false;
    }
    val = (uint8_t)(val << 1);
  }
  /* ACK clock: release SDA, read; 0 = ACK. */
  return i2c_read_bit(b) == 0u;
}

static uint8_t i2c_read_byte(const struct TouchBus *b, bool ack)
{
  uint8_t v = 0;

  for (uint8_t i = 0; i < 8u; i++) {
    v = (uint8_t)((v << 1) | i2c_read_bit(b));
  }
  (void)i2c_write_bit(b, ack ? 0u : 1u); /* master ACK=0 to continue, NACK=1 to stop */
  return v;
}

/* ---- GT911 register access ---- */

static bool gt911_write_reg_once(const struct TouchBus *b, uint8_t addr7, uint16_t reg,
                                 const uint8_t *buf, uint8_t len, bool reg_high_first)
{
  i2c_start(b);
  if (!i2c_write_byte(b, (uint8_t)((addr7 << 1) | 0u))) {
    goto fail;
  }
  if (reg_high_first) {
    if (!i2c_write_byte(b, (uint8_t)(reg >> 8)) || !i2c_write_byte(b, (uint8_t)(reg & 0xFFu))) {
      goto fail;
    }
  } else {
    if (!i2c_write_byte(b, (uint8_t)(reg & 0xFFu)) || !i2c_write_byte(b, (uint8_t)(reg >> 8))) {
      goto fail;
    }
  }
  for (uint8_t i = 0; i < len; i++) {
    if (!i2c_write_byte(b, buf[i])) {
      goto fail;
    }
  }
  i2c_stop(b);
  return true;
fail:
  i2c_stop(b);
  return false;
}

static bool gt911_read_reg_once(const struct TouchBus *b, uint8_t addr7, uint16_t reg,
                                uint8_t *buf, uint8_t len, bool reg_high_first)
{
  i2c_start(b);
  if (!i2c_write_byte(b, (uint8_t)((addr7 << 1) | 0u))) {
    goto fail;
  }
  if (reg_high_first) {
    if (!i2c_write_byte(b, (uint8_t)(reg >> 8)) || !i2c_write_byte(b, (uint8_t)(reg & 0xFFu))) {
      goto fail;
    }
  } else {
    if (!i2c_write_byte(b, (uint8_t)(reg & 0xFFu)) || !i2c_write_byte(b, (uint8_t)(reg >> 8))) {
      goto fail;
    }
  }
  /* Repeated start for the read phase. */
  i2c_start(b);
  if (!i2c_write_byte(b, (uint8_t)((addr7 << 1) | 1u))) {
    goto fail;
  }
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = i2c_read_byte(b, i < (uint8_t)(len - 1u));
  }
  i2c_stop(b);
  return true;
fail:
  i2c_stop(b);
  return false;
}

static bool gt911_write_reg(const struct TouchBus *b, uint8_t addr7, uint16_t reg,
                            const uint8_t *buf, uint8_t len, bool reg_high_first)
{
  for (uint8_t attempt = 0; attempt < GT911_I2C_RETRIES; attempt++) {
    if (gt911_write_reg_once(b, addr7, reg, buf, len, reg_high_first)) {
      return true;
    }
    k_busy_wait(500);
  }
  return false;
}

static bool gt911_read_reg(const struct TouchBus *b, uint8_t addr7, uint16_t reg,
                           uint8_t *buf, uint8_t len, bool reg_high_first)
{
  for (uint8_t attempt = 0; attempt < GT911_I2C_RETRIES; attempt++) {
    if (gt911_read_reg_once(b, addr7, reg, buf, len, reg_high_first)) {
      return true;
    }
    k_busy_wait(500);
  }
  return false;
}

static bool gt911_product_id_match(const uint8_t *id)
{
  return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

static bool gt911_probe_product(const struct TouchBus *b, uint8_t addr7, uint8_t *reg_high_first)
{
  uint8_t id[4];

  if (gt911_read_reg(b, addr7, GT911_REG_PID, id, 4, false) && gt911_product_id_match(id)) {
    *reg_high_first = 0;
    return true;
  }
  if (gt911_read_reg(b, addr7, GT911_REG_PID, id, 4, true) && gt911_product_id_match(id)) {
    *reg_high_first = 1;
    return true;
  }
  return false;
}

static void gt911_clear_status(const struct TouchBus *b, uint8_t addr7, bool reg_high_first)
{
  uint8_t z = 0;

  (void)gt911_write_reg(b, addr7, GT911_REG_STATUS, &z, 1, reg_high_first);
}

/* Config helpers -------------------------------------------------------- */

static bool touch_get_bus(const struct TouchController *tc, struct TouchBus *out)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  uint8_t bid = tc->bus_id;

  if (cfg == NULL) {
    return false;
  }
  if (bid == 0xFFu || bid >= cfg->data_bus_count) {
    return false; /* nRF54 has no implicit shared I2C; an explicit data_bus is required */
  }
  const struct DataBus *bus = &cfg->data_buses[bid];
  if (bus->bus_type != OD_BUS_TYPE_I2C || bus->pin_1 == 0xFFu || bus->pin_2 == 0xFFu) {
    return false;
  }
  out->scl = bus->pin_1;
  out->sda = bus->pin_2;
  return true;
}

static void touch_apply_enable_pin(const struct TouchController *tc)
{
  if (tc->enable_pin == 0u || tc->enable_pin == 0xFFu) {
    return;
  }
  nrf54_gpio_configure_output(tc->enable_pin, true);
}

static void gt911_int_wake(const struct TouchController *tc)
{
  if (tc->int_pin == 0xFFu) {
    return;
  }
  nrf54_gpio_configure_output(tc->int_pin, true);
  k_msleep(10);
  nrf54_gpio_configure_input(tc->int_pin, true, false);
}

/*
 * Hardware reset matching the reference timing/order: INT selects the address
 * (high before RST release => 0x14, low => 0x5D). Without an external RST
 * pull-up we drive RST HIGH to release it.
 */
static void gt911_hw_reset(const struct TouchController *tc, bool int_low_for_addr_5d)
{
  if (tc->rst_pin == 0xFFu) {
    return;
  }
  if (tc->int_pin == 0xFFu) {
    nrf54_gpio_configure_output(tc->rst_pin, false);
    k_msleep(10);
    nrf54_gpio_write(tc->rst_pin, true);
    k_msleep(60);
    return;
  }
  k_msleep(1);
  nrf54_gpio_configure_output(tc->int_pin, false);
  nrf54_gpio_configure_output(tc->rst_pin, false);
  k_msleep(11);
  nrf54_gpio_write(tc->int_pin, int_low_for_addr_5d ? false : true);
  k_busy_wait(110);
  nrf54_gpio_write(tc->rst_pin, true);
  k_msleep(6);
  nrf54_gpio_write(tc->int_pin, false);
  k_msleep(51);
  nrf54_gpio_write(tc->rst_pin, true);
  nrf54_gpio_configure_input(tc->int_pin, true, false);
}

static uint8_t gt911_resolve_and_init(const struct TouchController *tc, struct TouchRuntime *rt)
{
  const uint8_t a5d = 0x5Du;
  const uint8_t a14 = 0x14u;
  uint8_t want = tc->i2c_addr_7bit;

  if (want != 0u && want != 0xFFu) {
    if (tc->rst_pin != 0xFFu) {
      k_msleep(GT911_PRE_RESET_DELAY_MS);
      gt911_hw_reset(tc, want == a5d);
      k_msleep(GT911_POST_RESET_SETTLE_MS);
    } else {
      k_msleep(10);
    }
    if (gt911_probe_product(&rt->bus, want, &rt->reg_high_first)) {
      return want;
    }
    return 0;
  }

  if (tc->rst_pin != 0xFFu) {
    k_msleep(GT911_PRE_RESET_DELAY_MS);
    gt911_hw_reset(tc, true);
    k_msleep(GT911_POST_RESET_SETTLE_MS);
    if (gt911_probe_product(&rt->bus, a5d, &rt->reg_high_first)) {
      return a5d;
    }
    k_msleep(GT911_PRE_RESET_DELAY_MS);
    gt911_hw_reset(tc, false);
    k_msleep(GT911_POST_RESET_SETTLE_MS);
    if (gt911_probe_product(&rt->bus, a14, &rt->reg_high_first)) {
      return a14;
    }
  } else {
    if (gt911_probe_product(&rt->bus, a5d, &rt->reg_high_first)) {
      return a5d;
    }
    if (gt911_probe_product(&rt->bus, a14, &rt->reg_high_first)) {
      return a14;
    }
  }
  return 0;
}

static bool touch_reinit_gt911(const struct TouchController *tc, struct TouchRuntime *rt)
{
  uint8_t addr;

  if (tc->touch_ic_type != TOUCH_IC_GT911) {
    return false;
  }
  if (!touch_get_bus(tc, &rt->bus)) {
    return false;
  }
  if (tc->touch_data_start_byte > 6u) {
    return false;
  }
  touch_apply_enable_pin(tc);
  /* Idle-high bus before first START. */
  line_release(rt->bus.scl);
  line_release(rt->bus.sda);

  addr = gt911_resolve_and_init(tc, rt);
  if (addr == 0u) {
    rt->ok = 0;
    return false;
  }
  rt->addr7 = addr;
  rt->ok = 1;
  rt->disabled = 0;
  rt->i2c_fail_streak = 0;
  rt->touch_latched = 0;
  gt911_clear_status(&rt->bus, addr, rt->reg_high_first != 0);
  gt911_int_wake(tc);
  return true;
}

static bool touch_light_resume_gt911(const struct TouchController *tc, struct TouchRuntime *rt)
{
  uint8_t id[4];

  if (tc->touch_ic_type != TOUCH_IC_GT911 || !rt->ok || rt->addr7 == 0u) {
    return false;
  }
  if (!touch_get_bus(tc, &rt->bus)) {
    return false;
  }
  line_release(rt->bus.scl);
  line_release(rt->bus.sda);
  if (!gt911_read_reg(&rt->bus, rt->addr7, GT911_REG_PID, id, 4, rt->reg_high_first != 0) ||
      !gt911_product_id_match(id)) {
    return false;
  }
  rt->i2c_fail_streak = 0;
  rt->touch_latched = 0;
  gt911_clear_status(&rt->bus, rt->addr7, rt->reg_high_first != 0);
  gt911_int_wake(tc);
  return true;
}

static void apply_touch_map(const struct TouchController *tc, uint16_t *x, uint16_t *y)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  uint16_t w = 0;
  uint16_t h = 0;

  if (tc->flags & TOUCH_FLAG_SWAP_XY) {
    uint16_t tmp = *x;
    *x = *y;
    *y = tmp;
  }
  if (cfg != NULL && tc->display_instance < cfg->display_count) {
    w = cfg->displays[tc->display_instance].pixel_width;
    h = cfg->displays[tc->display_instance].pixel_height;
  }
  if ((tc->flags & TOUCH_FLAG_INVERT_X) && w > 0u) {
    *x = (w > *x) ? (uint16_t)(w - 1u - *x) : 0u;
  }
  if ((tc->flags & TOUCH_FLAG_INVERT_Y) && h > 0u) {
    *y = (h > *y) ? (uint16_t)(h - 1u - *y) : 0u;
  }
  if (w > 0u && *x >= w) {
    *x = (uint16_t)(w - 1u);
  }
  if (h > 0u && *y >= h) {
    *y = (uint16_t)(h - 1u);
  }
}

/* Public API ------------------------------------------------------------ */

void opendisplay_touch_init(void)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();

  memset(s_touch_rt, 0, sizeof(s_touch_rt));
  s_any_initialized = false;
  s_last_process_ms = 0;

  if (cfg == NULL || !cfg->loaded || cfg->touch_controller_count == 0u) {
    return;
  }

  for (uint8_t i = 0; i < cfg->touch_controller_count && i < 4u; i++) {
    const struct TouchController *tc = &cfg->touch_controllers[i];
    struct TouchRuntime *rt = &s_touch_rt[i];

    if (tc->touch_ic_type == TOUCH_IC_NONE) {
      continue;
    }
    if (tc->touch_ic_type != TOUCH_IC_GT911) {
      printf("[OD] touch[%u]: skipped (only GT911 implemented, got %u)\r\n", (unsigned)i,
             (unsigned)tc->touch_ic_type);
      continue;
    }
    if (tc->touch_data_start_byte > 6u) {
      printf("[OD] touch[%u]: touch_data_start_byte must be 0-6\r\n", (unsigned)i);
      continue;
    }
    if (!touch_get_bus(tc, &rt->bus)) {
      printf("[OD] touch[%u]: no valid I2C data_bus (bus_id=%u)\r\n", (unsigned)i,
             (unsigned)tc->bus_id);
      continue;
    }
    if (!touch_reinit_gt911(tc, rt)) {
      printf("[OD] touch[%u]: GT911 init failed (SCL=0x%02X SDA=0x%02X)\r\n", (unsigned)i,
             (unsigned)rt->bus.scl, (unsigned)rt->bus.sda);
      continue;
    }
    rt->last_poll_ms = k_uptime_get_32();
    s_any_initialized = true;
    printf("[OD] touch[%u]: GT911 @0x%02X %s SCL=0x%02X SDA=0x%02X byte=%u\r\n", (unsigned)i,
           (unsigned)rt->addr7, rt->reg_high_first ? "BE" : "LE", (unsigned)rt->bus.scl,
           (unsigned)rt->bus.sda, (unsigned)tc->touch_data_start_byte);
  }
}

bool opendisplay_touch_gpio_is_touch_int(uint8_t pin)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();

  if (pin == 0xFFu || cfg == NULL) {
    return false;
  }
  for (uint8_t i = 0; i < cfg->touch_controller_count && i < 4u; i++) {
    const struct TouchController *tc = &cfg->touch_controllers[i];
    if (tc->touch_ic_type == TOUCH_IC_GT911 && tc->int_pin == pin) {
      return true;
    }
  }
  return false;
}

void opendisplay_touch_resume_after_refresh(void)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();

  if (!s_any_initialized || cfg == NULL) {
    return;
  }
  for (uint8_t i = 0; i < cfg->touch_controller_count && i < 4u; i++) {
    const struct TouchController *tc = &cfg->touch_controllers[i];
    struct TouchRuntime *rt = &s_touch_rt[i];

    if (tc->touch_ic_type != TOUCH_IC_GT911 || rt->disabled) {
      continue;
    }
    if (touch_light_resume_gt911(tc, rt)) {
      printf("[OD] touch[%u]: light resume after EPD @0x%02X\r\n", (unsigned)i,
             (unsigned)rt->addr7);
    } else if (touch_reinit_gt911(tc, rt)) {
      printf("[OD] touch[%u]: reinit after EPD @0x%02X\r\n", (unsigned)i, (unsigned)rt->addr7);
    } else {
      printf("[OD] touch[%u]: resume failed after EPD refresh\r\n", (unsigned)i);
    }
  }
}

void opendisplay_touch_process(void)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  uint32_t now;

  if (!s_any_initialized || cfg == NULL || cfg->touch_controller_count == 0u) {
    return;
  }
  now = k_uptime_get_32();
  if ((uint32_t)(now - s_last_process_ms) < TOUCH_PROCESS_MIN_INTERVAL_MS) {
    return;
  }
  s_last_process_ms = now;

  for (uint8_t i = 0; i < cfg->touch_controller_count && i < 4u; i++) {
    const struct TouchController *tc = &cfg->touch_controllers[i];
    struct TouchRuntime *rt = &s_touch_rt[i];
    uint8_t interval;
    const bool rh = rt->reg_high_first != 0;
    uint8_t st = 0;
    uint8_t n;
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t tid = 0;
    bool changed;
    uint8_t s;

    if (tc->touch_ic_type != TOUCH_IC_GT911 || !rt->ok || rt->disabled) {
      continue;
    }
    interval = tc->poll_interval_ms ? tc->poll_interval_ms : (uint8_t)TOUCH_PROCESS_MIN_INTERVAL_MS;
    if ((uint32_t)(now - rt->last_poll_ms) < interval) {
      continue;
    }

    if (!gt911_read_reg(&rt->bus, rt->addr7, GT911_REG_STATUS, &st, 1, rh)) {
      if (rt->i2c_fail_streak < 255u) {
        rt->i2c_fail_streak++;
      }
      if (rt->i2c_fail_streak >= TOUCH_I2C_FAIL_DISABLE_THRESHOLD) {
        rt->disabled = 1;
        rt->ok = 0;
        printf("[OD] touch[%u]: disabled (too many I2C read failures)\r\n", (unsigned)i);
      }
      rt->last_poll_ms = now;
      continue;
    }
    rt->i2c_fail_streak = 0;
    rt->last_poll_ms = now;

    if ((st & GT911_STATUS_BUFFER_READY) == 0u) {
      continue;
    }
    n = (uint8_t)(st & 0x0Fu);
    if (n > GT911_MAX_CONTACTS) {
      gt911_clear_status(&rt->bus, rt->addr7, rh);
      continue;
    }

    if (n > 0u) {
      uint8_t p[8];
      if (!gt911_read_reg(&rt->bus, rt->addr7, GT911_REG_POINT1, p, 8, rh)) {
        if (rt->i2c_fail_streak < 255u) {
          rt->i2c_fail_streak++;
        }
        if (rt->i2c_fail_streak >= TOUCH_I2C_FAIL_DISABLE_THRESHOLD) {
          rt->disabled = 1;
          rt->ok = 0;
          printf("[OD] touch[%u]: disabled (too many I2C read failures)\r\n", (unsigned)i);
        }
        continue;
      }
      tid = p[0];
      x = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
      y = (uint16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
      rt->touch_latched = 1;
    } else {
      x = rt->last_x;
      y = rt->last_y;
      tid = rt->last_id;
    }
    gt911_clear_status(&rt->bus, rt->addr7, rh);

    if (n > 0u) {
      apply_touch_map(tc, &x, &y);
    }

    changed = (n != rt->last_count) ||
              (n > 0u && ((x != rt->last_x) || (y != rt->last_y) || (tid != rt->last_id)));
    rt->last_count = n;
    rt->last_x = x;
    rt->last_y = y;
    rt->last_id = tid;

    s = tc->touch_data_start_byte;
    if ((uint16_t)s + 5u > 11u) {
      continue;
    }
    /* MSD 5-byte block, byte-identical to the Arduino reference:
     *   byte0 low nibble  = contact count 1-5 (down) / 6 (released, last xy kept)
     *                       / 0 (never touched, block cleared)
     *   byte0 high nibble = last track id
     *   bytes 1-4         = last X (LE), last Y (LE) */
    if (n == 0u && !rt->touch_latched) {
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 0u), 0u);
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 1u), 0u);
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 2u), 0u);
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 3u), 0u);
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 4u), 0u);
    } else {
      uint8_t low = (n == 0u) ? 6u : (uint8_t)(n & 0x0Fu);
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 0u),
                                       (uint8_t)(low | ((tid & 0x0Fu) << 4)));
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 1u), (uint8_t)(x & 0xFFu));
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 2u), (uint8_t)(x >> 8));
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 3u), (uint8_t)(y & 0xFFu));
      opendisplay_ble_set_dynamic_byte((uint8_t)(s + 4u), (uint8_t)(y >> 8));
    }

    if (changed) {
      opendisplay_ble_update_msd(true);
      opendisplay_ble_boost_advertising();
      printf("[OD] touch[%u]: n=%u id=%u (%u,%u)\r\n", (unsigned)i, (unsigned)n, (unsigned)tid,
             (unsigned)x, (unsigned)y);
    }
  }
}
