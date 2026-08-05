#ifndef OPENDISPLAY_TOUCH_H
#define OPENDISPLAY_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise all configured GT911 touch controllers (packet 0x28). Call after
 * the config is loaded, next to opendisplay_button_init(). */
void opendisplay_touch_init(void);

/* Poll active controllers and push touch state into the MSD dynamic bytes.
 * Non-blocking; call from the main loop next to opendisplay_button_process(). */
void opendisplay_touch_process(void);

/* True if `pin` (compact (port<<4)|pin encoding) is a configured GT911 INT pin,
 * so button init can leave it reserved for touch. */
bool opendisplay_touch_gpio_is_touch_int(uint8_t pin);

/* Re-establish the GT911 after an EPD refresh: a lightweight product-ID probe
 * first, falling back to a full hardware reset + re-init only if that fails
 * (mirrors OpenDisplay-Firmware touch_input.cpp PR #75 behaviour). */
void opendisplay_touch_resume_after_refresh(void);

#ifdef __cplusplus
}
#endif

#endif
