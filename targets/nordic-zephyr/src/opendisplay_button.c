#include "opendisplay_button.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_structs.h"
#include "opendisplay_touch.h"
#include "nrf54_gpio.h"

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

/* Set from GPIO ISR context (both-edges interrupt). The ISR does NO I2C/BLE
 * work: it only raises this flag, which opendisplay_button_process() consumes on
 * the main loop. Polling remains as a fallback in case an edge is missed. */
static volatile bool s_button_irq_pending;

static void button_irq_handler(void)
{
  s_button_irq_pending = true;
}

static bool read_logical_pressed(const ButtonState *btn)
{
  bool level = nrf54_gpio_read(btn->pin) != 0;
  return btn->inverted ? !level : level;
}

void opendisplay_button_init(void)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();

  memset(s_buttons, 0, sizeof(s_buttons));
  s_button_count = 0;
  if (cfg == NULL || !cfg->loaded || cfg->binary_input_count == 0u) {
    return;
  }

  for (uint8_t instance_idx = 0; instance_idx < cfg->binary_input_count; instance_idx++) {
    const struct BinaryInputs *input = &cfg->binary_inputs[instance_idx];
    uint8_t *instance_pins[8] = {
      &input->reserved_pin_1, &input->reserved_pin_2, &input->reserved_pin_3, &input->reserved_pin_4,
      &input->reserved_pin_5, &input->reserved_pin_6, &input->reserved_pin_7, &input->reserved_pin_8,
    };

    if (input->input_type != 1u || input->button_data_byte_index > 10u) {
      continue;
    }

    for (uint8_t pin_idx = 0; pin_idx < 8u; pin_idx++) {
      if (input->input_flags != 0u && (input->input_flags & (1u << pin_idx)) == 0u) {
        continue;
      }
      uint8_t pin = *instance_pins[pin_idx];
      if (pin == 0xFFu || s_button_count >= MAX_BUTTONS) {
        continue;
      }
      if (opendisplay_touch_gpio_is_touch_int(pin)) {
        printf("[OD] button: skip pin 0x%02X (reserved for GT911 INT)\r\n", (unsigned)pin);
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
      nrf54_gpio_configure_input(pin, pull_up, pull_down);
      btn->current_state = read_logical_pressed(btn) ? 1u : 0u;
      btn->initialized = true;
      /* Attach a both-edges interrupt (reference uses CHANGE, device_control.cpp:604).
       * On failure we still have the polling path in _process(). */
      if (nrf54_gpio_configure_interrupt(pin, button_irq_handler) != 0) {
        printf("[OD] button pin=0x%02X interrupt setup failed; polling only\r\n",
               (unsigned)pin);
      }
      printf("[OD] button id=%u pin=0x%02X byte=%u pull=%s\r\n", (unsigned)btn->button_id,
             (unsigned)pin, (unsigned)btn->byte_index,
             pull_up ? "up" : (pull_down ? "down" : "none"));
    }
  }
}

void opendisplay_button_process(void)
{
  /* Consume any interrupt signal. The actual state change is detected by the
   * poll below (edge-agnostic), which also covers a missed/coalesced edge. */
  s_button_irq_pending = false;

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
    opendisplay_ble_boost_advertising();
    printf("[OD] button id=%u state=%u count=%u\r\n", (unsigned)btn->button_id,
           (unsigned)btn->current_state, (unsigned)btn->press_count);
  }
}
