#include "opendisplay_button.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "od_adv_app.h"
#include "opendisplay_config_parser.h"
#include "od_runtime_types.h"
#include "opendisplay_touch.h"
#include "od_gpio.h"

#include <stdio.h>
#include <string.h>

#define MAX_BUTTONS 8u

typedef struct {
  bool initialized;
  uint8_t button_id;
  uint8_t press_count;
  uint8_t current_state;
  uint8_t byte_index;
  uint8_t pin;
  bool inverted;
} ButtonState;

static ButtonState s_buttons[MAX_BUTTONS];
static uint8_t s_button_count;

static bool read_logical_pressed(const ButtonState *btn)
{
  bool level = od_gpio_read(btn->pin) != 0;
  return btn->inverted ? !level : level;
}

void opendisplay_button_init(void)
{
  const struct od_config *cfg = opendisplay_get_global_config();

  memset(s_buttons, 0, sizeof(s_buttons));
  s_button_count = 0;
  if (cfg == NULL || !cfg->loaded || cfg->binary_input_count == 0u) {
    return;
  }

  for (uint8_t instance_idx = 0; instance_idx < cfg->binary_input_count; instance_idx++) {
    const struct BinaryInputs *input = &cfg->binary_inputs[instance_idx];
    uint8_t *instance_pins[8] = {
      &input->input_pin_1, &input->input_pin_2, &input->input_pin_3, &input->input_pin_4,
      &input->input_pin_5, &input->input_pin_6, &input->input_pin_7, &input->input_pin_8,
    };

    if (input->input_type != 1u || input->button_data_byte_index > 10u) {
      continue;
    }

    for (uint8_t pin_idx = 0; pin_idx < 8u; pin_idx++) {
      if (input->pins_used != 0u && (input->pins_used & (1u << pin_idx)) == 0u) {
        continue;
      }
      uint8_t pin = *instance_pins[pin_idx];
      if (pin == 0xFFu) {
        continue;
      }
      /* The contract allows 4 blocks x 8 pins; this target tracks MAX_BUTTONS. Refusing
       * quietly leaves a host with a configured button that never reports and no way to
       * find out. */
      if (s_button_count >= MAX_BUTTONS) {
        od_log_warn("button pin=0x%02X refused: %u configured, this target tracks %u",
               (unsigned)pin, (unsigned)(s_button_count + 1u), (unsigned)MAX_BUTTONS);
        continue;
      }
      if (opendisplay_touch_gpio_is_touch_int(pin)) {
        od_log_info("button: skip pin 0x%02X (reserved for GT911 INT)", (unsigned)pin);
        continue;
      }

      ButtonState *btn = &s_buttons[s_button_count++];
      btn->button_id = (uint8_t)((input->instance_number * 8u) + pin_idx);
      if (btn->button_id > 7u) {
        btn->button_id = (uint8_t)(btn->button_id % 8u);
      }
      btn->byte_index = input->button_data_byte_index;
      btn->pin = pin;
      btn->inverted = (input->invert & (1u << pin_idx)) != 0u;
      bool pull_up = (input->pullups & (1u << pin_idx)) != 0u;
      bool pull_down = (input->pulldowns & (1u << pin_idx)) != 0u;
      od_gpio_configure_input(pin, pull_up, pull_down);
      btn->current_state = read_logical_pressed(btn) ? 1u : 0u;
      btn->initialized = true;
      od_log_info("button id=%u pin=0x%02X byte=%u pull=%s", (unsigned)btn->button_id,
             (unsigned)pin, (unsigned)btn->byte_index,
             pull_up ? "up" : (pull_down ? "down" : "none"));
    }
  }
}

/* Detection is by level comparison on each call, so a press and release that both complete
 * between two calls is not reported. The caller sets that window: opendisplay_ble_process() runs
 * every 10 ms while connected, but only every 500 ms to 1 s while idle-advertising, depending on
 * whether a sleep_timeout_ms is configured (main.c idle_delay_ms). Closing it needs the edge
 * recorded in ISR context -- docs/FOLLOWUPS.md § 12. */
void opendisplay_button_process(void)
{
  for (uint8_t i = 0; i < s_button_count; i++) {
    ButtonState *btn = &s_buttons[i];
    bool pressed;
    uint8_t logical_state;
    uint8_t button_data;

    if (!btn->initialized) {
      continue;
    }

    pressed = read_logical_pressed(btn);
    logical_state = pressed ? 1u : 0u;
    if (logical_state == btn->current_state) {
      continue;
    }

    btn->current_state = logical_state;
    if (pressed) {
      btn->press_count = (uint8_t)((btn->press_count + 1u) & 0x0Fu);
    }

    button_data = (uint8_t)((btn->button_id & 0x07u) | ((btn->press_count & 0x0Fu) << 3) |
                            ((btn->current_state & 0x01u) << 7));
    opendisplay_ble_set_dynamic_byte(btn->byte_index, button_data);
    opendisplay_ble_update_msd(true);
    od_adv_app_boost();
    od_log_info("button id=%u state=%u count=%u", (unsigned)btn->button_id,
           (unsigned)btn->current_state, (unsigned)btn->press_count);
  }
}
