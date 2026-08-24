/* od_touch_app -- what a shared GT911 driver cannot do for itself.
 *
 * GPIO and the yielding delays, per T5: "GPIO reset and IRQ handling remain behind a touch/GPIO
 * seam". There is no shared GPIO HAL, and inventing one to serve a single driver would be a
 * larger change than the promotion it exists for -- ESP32 calls od_hal_gpio, Nordic calls
 * od_gpio, and this seam is the one place that difference is spelled out.
 *
 * THE RESET SEQUENCE IS WHY set_mode_output AND write ARE SEPARATE. Both pads are made outputs
 * BEFORE either is driven, and INT's level at RST's rising edge selects the controller's I2C
 * address. A combined "configure as output at this level" call would reorder that and change
 * which address the part answers on.
 */

#ifndef OD_TOUCH_APP_H
#define OD_TOUCH_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Yielding where the target has a scheduler; the microsecond one is a busy wait by nature. */
void od_touch_app_delay_ms(uint16_t ms);
void od_touch_app_delay_us(uint32_t us);

void od_touch_app_gpio_set_mode_output(uint8_t pin);
void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up);
void od_touch_app_gpio_write(uint8_t pin, bool level_high);
int  od_touch_app_gpio_read(uint8_t pin);

/* One byte of the advertisement's dynamic block. Bounds are the caller's. */
void od_touch_app_msd_write(uint8_t index, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* OD_TOUCH_APP_H */
