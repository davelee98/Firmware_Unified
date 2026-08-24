/* BG22 adapter for the shared LED runner.
 *
 * The pattern machine, the software PWM and the group/loop accounting are shared/core/od_led.c.
 * What is target-specific and stays here: decoding the config's encoded pin bytes onto EFR32
 * ports, the initial pin configuration, and arming the sleeptimer for the delay the machine asks
 * for. This is a superloop, so "arm a timer" is also what lets the part idle between steps.
 */

#include "opendisplay_led.h"

#include "od_led.h"
#include "od_led_app.h"
#include "od_hal_time.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "opendisplay_runtime.h"

#include "em_cmu.h"
#include "em_gpio.h"
#include "sl_sleeptimer.h"

#include <stdbool.h>
#include <string.h>

#define LED_FLAG_INVERT_RED    0x01u
#define LED_FLAG_INVERT_GREEN  0x02u
#define LED_FLAG_INVERT_BLUE   0x04u
#define LED_FLAG_INVERT_LED4   0x08u

static sl_sleeptimer_timer_handle_t s_led_timer;
static volatile bool s_timer_due;
static bool s_armed;
static bool s_running;

static bool od_led_pin_decode(uint8_t v, GPIO_Port_TypeDef *port_out, uint8_t *pin_out)
{
  if (v == GPIO_PIN_UNUSED) {
    return false;
  }
  unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
  unsigned pn = (unsigned)(v & 0x0Fu);
  if (pr > (unsigned)GPIO_PORT_MAX) {
    return false;
  }
  if (pn > 15u) {
    return false;
  }
  *port_out = (GPIO_Port_TypeDef)(gpioPortA + pr);
  *pin_out = (uint8_t)pn;
  return true;
}

static void od_gpio_mode_push_pull(uint8_t cfg)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;

  if (!od_led_pin_decode(cfg, &port, &pin)) {
    return;
  }
  GPIO_PinModeSet(port, pin, gpioModePushPull, 0);
}

/* ------------------------------------------------------------------ the od_led seam --- */

void od_led_app_write(uint8_t pin_cfg, bool level_high)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;

  if (!od_led_pin_decode(pin_cfg, &port, &pin)) {
    return;
  }
  if (level_high) {
    GPIO_PinOutSet(port, pin);
  } else {
    GPIO_PinOutClear(port, pin);
  }
}

static struct LedConfig *led_instance(uint8_t instance)
{
  struct GlobalConfig *gc = (struct GlobalConfig *)opendisplay_get_global_config();

  if (gc == NULL || !gc->loaded || instance >= gc->led_count) {
    return NULL;
  }
  return &gc->leds[instance];
}

uint8_t od_led_app_mode(uint8_t instance)
{
  const struct LedConfig *led = led_instance(instance);

  return (led == NULL) ? 0u : (uint8_t)(led->reserved[0] & 0x0Fu);
}

void od_led_app_finished(uint8_t instance)
{
  struct LedConfig *led = led_instance(instance);

  if (led != NULL) {
    led->reserved[0] = 0x00u;
  }
}

/* ----------------------------------------------------------------------- scheduling --- */

static void led_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  s_timer_due = true;
}

static void led_timer_stop(void)
{
  (void)sl_sleeptimer_stop_timer(&s_led_timer);
  s_timer_due = false;
  s_armed = false;
}

/* Arm for the delay the machine asked for. od_led_service() never returns 0, so there is no
 * due-immediately case to handle. */
static void led_arm(uint32_t delay_ms)
{
  led_timer_stop();
  s_armed = true;
  (void)sl_sleeptimer_start_timer_ms(&s_led_timer, delay_ms, led_timer_cb, NULL, 0u, 0u);
}

static void led_pump(void)
{
  const uint32_t delay_ms = od_led_service(od_hal_uptime_ms());

  if (delay_ms == OD_LED_IDLE) {
    led_timer_stop();
    s_running = false;
    return;
  }
  led_arm(delay_ms);
}

/* --------------------------------------------------------------------- the wire API --- */

void opendisplay_led_init(void)
{
  const struct GlobalConfig *gc = opendisplay_get_global_config();

  CMU_ClockEnable(cmuClock_GPIO, true);

  if (gc == NULL || !gc->loaded || gc->led_count == 0u) {
    return;
  }

  for (uint8_t i = 0; i < gc->led_count; i++) {
    const struct LedConfig *led = &gc->leds[i];
    bool inv_r = (led->led_flags & LED_FLAG_INVERT_RED) != 0u;
    bool inv_g = (led->led_flags & LED_FLAG_INVERT_GREEN) != 0u;
    bool inv_b = (led->led_flags & LED_FLAG_INVERT_BLUE) != 0u;
    bool inv_4 = (led->led_flags & LED_FLAG_INVERT_LED4) != 0u;

    if (led->led_1_r != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_1_r);
      od_led_app_write(led->led_1_r, inv_r);
    }
    if (led->led_2_g != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_2_g);
      od_led_app_write(led->led_2_g, inv_g);
    }
    if (led->led_3_b != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_3_b);
      od_led_app_write(led->led_3_b, inv_b);
    }
    if (led->led_4 != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_4);
      od_led_app_write(led->led_4, inv_4);
    }
  }
}

int opendisplay_led_activate(uint8_t instance, const uint8_t *rest, uint16_t rest_len)
{
  struct LedConfig *led = led_instance(instance);
  struct od_led_pins pins;
  int rc;

  if (led == NULL) {
    return 2;
  }
  if (rest_len >= OD_LED_PATTERN_LEN && rest != NULL) {
    memcpy(led->reserved, rest, OD_LED_PATTERN_LEN);
  }

  pins.r = led->led_1_r;
  pins.g = led->led_2_g;
  pins.b = led->led_3_b;
  pins.flags = led->led_flags;

  led_timer_stop();
  rc = od_led_activate(instance, &pins, led->reserved, od_hal_uptime_ms());
  if (rc != 0) {
    /* Mode is not "run": the deployed contract answers success and leaves the LEDs off. */
    (void)od_led_stop(0u, false);
    s_running = false;
    return 0;
  }
  s_running = true;
  led_pump();
  return 0;
}

int opendisplay_led_stop(uint8_t instance, bool instance_given)
{
  const int rc = od_led_stop(instance, instance_given);

  if (rc != 0) {
    return rc;
  }
  led_timer_stop();
  s_running = false;
  return 0;
}

void opendisplay_led_process(void)
{
  if (!s_running) {
    return;
  }
  if (s_armed && !s_timer_due) {
    return;
  }
  s_timer_due = false;
  led_pump();
}
