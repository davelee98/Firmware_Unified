#include "opendisplay_button.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_structs.h"
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

/*
 * THE ISR MUST LATCH THE PRESS, not merely announce that something happened.
 *
 * It used to set a bare s_button_irq_pending flag which opendisplay_button_process() cleared
 * on entry and never read -- so the interrupt did literally nothing, and detection fell
 * entirely to the poll in _process(). While DISCONNECTED (the normal state when someone
 * presses a button) main()'s idle_delay_ms() runs that poll ONCE PER 1000 ms chunk, so a
 * human press of 100-300 ms began and ended between two polls and was invisible. That is why
 * presses produced neither a log line nor an MSD update.
 *
 * These arrays are the fix: the ISR samples the pin itself and records the edge, so a press
 * shorter than the poll interval still survives to be published. s_irq_presses is a COUNT,
 * not a flag, so two quick presses between polls are not collapsed into one.
 */
static volatile uint8_t s_irq_presses[MAX_BUTTONS];
static volatile uint8_t s_irq_level[MAX_BUTTONS];
static volatile bool s_irq_seen[MAX_BUTTONS];

/* Woken by the ISR so the idle loop stops sleeping and publishes promptly instead of waiting
 * out the rest of its chunk. Declared here, consumed by main() via opendisplay_button_wait(). */
static K_SEM_DEFINE(s_button_evt, 0, 1);

static bool read_logical_pressed(const ButtonState *btn)
{
  bool level = od_gpio_read(btn->pin) != 0;
  return btn->inverted ? !level : level;
}

/*
 * Runs in GPIO ISR context: NO BLE, NO I2C, NO logging. It only reads pins and updates the
 * latches above. The shared handler takes no argument, so it samples every button -- at most
 * MAX_BUTTONS register reads, which is cheap enough for an ISR.
 */
static void button_irq_handler(void)
{
  for (uint8_t i = 0; i < s_button_count; i++) {
    ButtonState *btn = &s_buttons[i];
    bool pressed;

    if (!btn->initialized) {
      continue;
    }
    pressed = read_logical_pressed(btn);
    if (s_irq_seen[i] && (uint8_t)(pressed ? 1u : 0u) == s_irq_level[i]) {
      continue;
    }
    s_irq_seen[i] = true;
    s_irq_level[i] = pressed ? 1u : 0u;
    if (pressed && s_irq_presses[i] < 0xFFu) {
      s_irq_presses[i]++;
    }
  }
  k_sem_give(&s_button_evt);
}

void opendisplay_button_wait(uint32_t timeout_ms)
{
  (void)k_sem_take(&s_button_evt, K_MSEC(timeout_ms));
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
      s_irq_level[s_button_count - 1u] = btn->current_state;
      s_irq_seen[s_button_count - 1u] = true;
      s_irq_presses[s_button_count - 1u] = 0u;
      btn->initialized = true;
      /* Attach a both-edges interrupt (reference uses CHANGE, device_control.cpp:604).
       * On failure we still have the polling path in _process(). */
      if (od_gpio_configure_interrupt(pin, button_irq_handler) != 0) {
        od_log_info("button pin=0x%02X interrupt setup failed; polling only",
               (unsigned)pin);
      }
      od_log_info("button id=%u pin=0x%02X byte=%u pull=%s", (unsigned)btn->button_id,
             (unsigned)pin, (unsigned)btn->byte_index,
             pull_up ? "up" : (pull_down ? "down" : "none"));
    }
  }
}

void opendisplay_button_process(void)
{
  for (uint8_t i = 0; i < s_button_count; i++) {
    ButtonState *btn = &s_buttons[i];
    bool pressed;
    uint8_t logical_state;
    uint8_t button_data;
    uint8_t irq_presses;

    if (!btn->initialized) {
      continue;
    }

    /*
     * Take what the ISR latched FIRST, then fall back to a live read. The order matters: a
     * press that has already been released is invisible to the live read, and it is exactly
     * that case the poll used to lose.
     */
    irq_presses = s_irq_presses[i];
    s_irq_presses[i] = 0u;

    pressed = read_logical_pressed(btn);
    logical_state = pressed ? 1u : 0u;
    if (irq_presses == 0u && logical_state == btn->current_state) {
      continue;
    }

    btn->current_state = logical_state;
    if (irq_presses > 0u) {
      /* Count every press the ISR saw, so two taps between polls are not collapsed. */
      btn->press_count = (uint8_t)((btn->press_count + irq_presses) & 0x0Fu);
    } else if (pressed) {
      btn->press_count = (uint8_t)((btn->press_count + 1u) & 0x0Fu);
    }

    button_data = (uint8_t)((btn->button_id & 0x07u) | ((btn->press_count & 0x0Fu) << 3) |
                            ((btn->current_state & 0x01u) << 7));
    opendisplay_ble_set_dynamic_byte(btn->byte_index, button_data);
    opendisplay_ble_update_msd(true);
    opendisplay_ble_boost_advertising();
    od_log_info("button id=%u state=%u count=%u", (unsigned)btn->button_id,
           (unsigned)btn->current_state, (unsigned)btn->press_count);
  }
}
