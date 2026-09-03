/* Nordic adapter for the shared LED runner.
 *
 * The pattern machine, the software PWM and the group/loop accounting are shared/core/od_led.c.
 * What stays here: the initial pin configuration and arming a k_timer for the delay the machine
 * asks for, so the display thread is free between steps.
 */

#include "opendisplay_led.h"

#include "od_led.h"
#include "od_led_app.h"
#include "od_gpio.h"
#include "od_hal_time.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "od_runtime_types.h"

#include <string.h>
#include <zephyr/kernel.h>

#define LED_FLAG_INVERT_RED    0x01u
#define LED_FLAG_INVERT_GREEN  0x02u
#define LED_FLAG_INVERT_BLUE   0x04u

static struct k_timer s_led_timer;
static volatile bool s_timer_due;
static bool s_armed;
static bool s_running;

/* ------------------------------------------------------------------ the od_led seam --- */

void od_led_app_write(uint8_t pin_cfg, bool level_high)
{
	if (pin_cfg == OD_PIN_UNUSED) {
		return;
	}
	od_gpio_write(pin_cfg, level_high);
}

static struct LedConfig *led_instance(uint8_t instance)
{
	struct od_config *gc = (struct od_config *)opendisplay_get_global_config();

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

static void led_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	s_timer_due = true;
}

static void led_timer_stop(void)
{
	k_timer_stop(&s_led_timer);
	s_timer_due = false;
	s_armed = false;
}

/* od_led_service() never returns 0, so there is no due-immediately case to handle. */
static void led_arm(uint32_t delay_ms)
{
	led_timer_stop();
	s_armed = true;
	k_timer_start(&s_led_timer, K_MSEC(delay_ms), K_NO_WAIT);
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
	const struct od_config *gc = opendisplay_get_global_config();

	k_timer_init(&s_led_timer, led_timer_cb, NULL);
	if (gc == NULL || !gc->loaded || gc->led_count == 0u) {
		return;
	}
	for (uint8_t i = 0; i < gc->led_count; i++) {
		const struct LedConfig *led = &gc->leds[i];

		if (led->led_1_r != OD_PIN_UNUSED) {
			od_gpio_configure_output(led->led_1_r,
						 (led->led_flags & LED_FLAG_INVERT_RED) != 0u);
		}
		if (led->led_2_g != OD_PIN_UNUSED) {
			od_gpio_configure_output(led->led_2_g,
						 (led->led_flags & LED_FLAG_INVERT_GREEN) != 0u);
		}
		if (led->led_3_b != OD_PIN_UNUSED) {
			od_gpio_configure_output(led->led_3_b,
						 (led->led_flags & LED_FLAG_INVERT_BLUE) != 0u);
		}
	}
}

int opendisplay_led_activate(uint8_t instance, const uint8_t *rest, uint16_t rest_len)
{
	struct LedConfig *led = led_instance(instance);
	struct od_led_pins pins;

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
	if (od_led_activate(instance, &pins, led->reserved, od_hal_uptime_ms()) != 0) {
		/* Mode is not "run": the deployed contract answers success with the LEDs off. */
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
