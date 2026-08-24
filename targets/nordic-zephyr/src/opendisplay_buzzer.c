/* Nordic adapter for the shared buzzer runner. */

#include "opendisplay_buzzer.h"

#include "od_buzzer.h"
#include "od_buzzer_app.h"
#include "od_gpio.h"
#include "od_hal_time.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "od_runtime_types.h"

#include <stdbool.h>
#include <zephyr/kernel.h>

_Static_assert(sizeof(struct BuzzerConfig) == 32, "BuzzerConfig must be 32 bytes");

static struct k_timer s_step_timer;
static struct k_timer s_tone_timer;
static volatile bool s_step_due;
static bool s_step_armed;
static bool s_running;

static struct {
	uint8_t drive_pin;
	bool running;
	bool high;
	uint32_t on_us;
	uint32_t off_us;
} s_tone;

/* ---------------------------------------------------------------- the hardware seam --- */

void od_buzzer_app_tone_stop(uint8_t drive_pin)
{
	s_tone.running = false;
	k_timer_stop(&s_tone_timer);
	s_tone.high = false;
	if (drive_pin != GPIO_PIN_UNUSED) {
		od_gpio_write(drive_pin, false);
	}
}

bool od_buzzer_app_tone_start(uint8_t drive_pin, uint32_t centihz, uint8_t duty_percent)
{
	uint32_t hz;
	uint32_t period_us;
	uint32_t on_us;

	od_buzzer_app_tone_stop(drive_pin);
	if (centihz == 0u || drive_pin == GPIO_PIN_UNUSED) {
		return false;
	}
	if (duty_percent == 0u || duty_percent > 100u) {
		duty_percent = 50u;
	}
	hz = (centihz + 50u) / 100u;
	if (hz == 0u) {
		return false;
	}
	period_us = 1000000u / hz;
	if (period_us < 2u) {
		period_us = 2u;
	}
	on_us = (period_us * (uint32_t)duty_percent) / 100u;
	if (on_us == 0u) {
		on_us = 1u;
	}
	if (on_us >= period_us) {
		on_us = period_us - 1u;
	}
	s_tone.drive_pin = drive_pin;
	s_tone.on_us = on_us;
	s_tone.off_us = period_us - on_us;
	s_tone.high = true;
	s_tone.running = true;
	od_gpio_write(drive_pin, true);
	k_timer_start(&s_tone_timer, K_USEC(on_us), K_NO_WAIT);
	return true;
}

void od_buzzer_app_enable_write(uint8_t enable_pin, bool level_high)
{
	if (enable_pin != GPIO_PIN_UNUSED) {
		od_gpio_write(enable_pin, level_high);
	}
}

/* ----------------------------------------------------------------------- timers --- */

static void step_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	s_step_due = true;
}

static void tone_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	if (!s_tone.running) {
		return;
	}
	if (s_tone.high) {
		od_gpio_write(s_tone.drive_pin, false);
		s_tone.high = false;
		k_timer_start(&s_tone_timer, K_USEC(s_tone.off_us), K_NO_WAIT);
	} else {
		od_gpio_write(s_tone.drive_pin, true);
		s_tone.high = true;
		k_timer_start(&s_tone_timer, K_USEC(s_tone.on_us), K_NO_WAIT);
	}
}

static void step_timer_stop(void)
{
	k_timer_stop(&s_step_timer);
	s_step_due = false;
	s_step_armed = false;
}

static void buzzer_pump(void)
{
	const uint32_t delay_ms = od_buzzer_service(od_hal_uptime_ms());

	if (delay_ms == OD_BUZZER_IDLE) {
		step_timer_stop();
		s_running = false;
		return;
	}
	step_timer_stop();
	s_step_armed = true;
	k_timer_start(&s_step_timer, K_MSEC(delay_ms), K_NO_WAIT);
}

/* --------------------------------------------------------------------- target API --- */

void opendisplay_buzzer_init(void)
{
	const struct od_config *gc = opendisplay_get_global_config();

	k_timer_init(&s_step_timer, step_timer_cb, NULL);
	k_timer_init(&s_tone_timer, tone_timer_cb, NULL);
	step_timer_stop();
	s_running = false;
	if (gc == NULL || !gc->loaded) {
		return;
	}
	for (uint8_t i = 0u; i < gc->passive_buzzer_count; i++) {
		const struct BuzzerConfig *b = &gc->passive_buzzers[i];

		if (b->drive_pin == GPIO_PIN_UNUSED) {
			continue;
		}
		od_gpio_configure_output(b->drive_pin, false);
		if (b->enable_pin != GPIO_PIN_UNUSED) {
			const bool active_high =
				(b->flags & OD_BUZZER_FLAG_ENABLE_ACTIVE_HIGH) != 0u;
			od_gpio_configure_output(b->enable_pin, !active_high);
		}
		od_log_info("buzzer %u drive=0x%02X enable=0x%02X duty=%u",
			(unsigned)b->instance_number, (unsigned)b->drive_pin,
			(unsigned)b->enable_pin, (unsigned)b->duty_percent);
	}
}

int opendisplay_buzzer_activate(const uint8_t *payload, uint16_t payload_len)
{
	const struct od_config *gc;
	const struct BuzzerConfig *b;
	struct od_buzzer_config config;
	uint8_t instance;
	int rc;

	if (payload == NULL || payload_len < 3u) {
		return 1;
	}
	instance = payload[0];
	gc = opendisplay_get_global_config();
	if (gc == NULL || !gc->loaded || instance >= gc->passive_buzzer_count) {
		return 2;
	}
	b = &gc->passive_buzzers[instance];
	if (b->drive_pin == GPIO_PIN_UNUSED) {
		return 3;
	}
	config.drive_pin = b->drive_pin;
	config.enable_pin = b->enable_pin;
	config.flags = b->flags;
	config.duty_percent = b->duty_percent;
	rc = od_buzzer_activate(&config, payload, payload_len, od_hal_uptime_ms());
	if (rc != 0) {
		return rc;
	}
	step_timer_stop();
	s_running = true;
	buzzer_pump();
	return 0;
}

void opendisplay_buzzer_process(void)
{
	if (!s_running || (s_step_armed && !s_step_due)) {
		return;
	}
	s_step_due = false;
	buzzer_pump();
}

void opendisplay_buzzer_stop(void)
{
	od_buzzer_stop();
	step_timer_stop();
	s_running = false;
}
