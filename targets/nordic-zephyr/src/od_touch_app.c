/* Nordic's half of od_touch_app.h, and the wiring that keeps the shared machine scheduled.
 *
 * FIRST CONSUMER OF THE GPIO IRQ SEAM (od_gpio.h). That seam was built dormant for this driver;
 * until now no board image linked a symbol from it.
 *
 * NO BOARD IN THIS FLEET HAS A TOUCH CONTROLLER, so everything here is compiled and never run.
 * docs/HARDWARE_VERIFICATION_CHECKLIST.md carries the open rows; do not cite a build as evidence.
 */

#include "od_touch_app.h"

#include "od_gpio.h"
#include "od_touch_gt911.h"
#include "opendisplay_ble.h"
#include "opendisplay_touch.h"

#include <zephyr/kernel.h>

/* One ISR per controller: od_gpio_config_irq() takes a bare handler, and the index has to come
 * from somewhere. The _arg variant exists and would collapse these, but it is what the button
 * port wants; keeping touch on the plain form leaves the two independent. */
static volatile uint8_t s_irq_mask;

static void touch_isr_0(void) { s_irq_mask |= 1u << 0; }
static void touch_isr_1(void) { s_irq_mask |= 1u << 1; }
static void touch_isr_2(void) { s_irq_mask |= 1u << 2; }
static void touch_isr_3(void) { s_irq_mask |= 1u << 3; }

static void (*const s_isrs[OD_TOUCH_MAX_CONTROLLERS])(void) = {
	touch_isr_0, touch_isr_1, touch_isr_2, touch_isr_3
};

void od_touch_app_delay_ms(uint16_t ms)
{
	k_msleep((int32_t)ms);
}

void od_touch_app_delay_us(uint32_t us)
{
	k_busy_wait(us);
}

void od_touch_app_gpio_set_mode_output(uint8_t pin)
{
	od_gpio_set_mode_output(pin);
}

void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up)
{
	od_gpio_configure_input(pin, pull_up, /*pull_down=*/false);
}

void od_touch_app_gpio_write(uint8_t pin, bool level_high)
{
	od_gpio_write(pin, level_high);
}

int od_touch_app_gpio_read(uint8_t pin)
{
	uint8_t port;
	uint8_t p;

	/* ASK BEFORE READING. od_gpio_read() folds every failure to 0 by design -- callers test it
	 * as a boolean and an errno would read as HIGH -- so "unreadable" and LOW are the same value
	 * there. The driver treats < 0 as "no level"; the charger seam is the standing example of
	 * what conflating the two costs (DIVERGENCE_MATRIX 21). */
	if (!od_pin_decode(pin, &port, &p)) {
		return -1;
	}
	return od_gpio_read(pin);
}

bool od_touch_app_gpio_attach_int(uint8_t idx, uint8_t pin)
{
	if (idx >= OD_TOUCH_MAX_CONTROLLERS) {
		return false;
	}
	return od_gpio_config_irq(pin, OD_GPIO_EDGE_FALLING, s_isrs[idx]) == 0;
}

void od_touch_app_gpio_detach_int(uint8_t pin)
{
	od_gpio_clear_irq(pin);
}

bool od_touch_app_bus_prepare(uint8_t bus_id)
{
	/* This target re-initialises the bus inside every I2C operation, so there is nothing to
	 * select here and nothing that can fail. ESP32 caches one live IDF bus and must re-select. */
	(void)bus_id;
	return true;
}

void od_touch_app_bus_invalidate(void)
{
	/* Nothing is cached, so nothing to drop -- see od_touch_app_bus_prepare(). */
}

void od_touch_app_msd_write(uint8_t index, uint8_t value)
{
	opendisplay_ble_set_dynamic_byte(index, value);
}

void od_touch_app_msd_publish(void)
{
	opendisplay_ble_update_msd(true);
}

/* ------------------------------------------------------------------ scheduling */

static uint32_t s_next_due_ms;

void opendisplay_touch_init(void)
{
	const uint32_t now = k_uptime_get_32();

	s_irq_mask = 0u;
	s_next_due_ms = now + od_touch_gt911_init(opendisplay_get_global_config(), now);
}

void opendisplay_touch_process(void)
{
	uint32_t now;
	uint8_t  mask;
	uint8_t  consumed = 0u;

	/* AN EDGE PULLS SERVICE FORWARD, so the mask is read before the deadline is tested. Gating
	 * on the deadline alone would leave a controller with a long configured interval unable to
	 * be serviced by its own interrupt until that interval expired. */
	od_gpio_irq_lock();
	mask = s_irq_mask;
	od_gpio_irq_unlock();

	now = k_uptime_get_32();
	if (mask == 0u && (int32_t)(now - s_next_due_ms) < 0) {
		return;
	}

	s_next_due_ms = now + od_touch_gt911_service(opendisplay_get_global_config(), now,
						     mask, &consumed);

	/* Clear exactly what was acted on, so a bit for a controller the machine did not reach
	 * survives to the next pass. A second edge from a controller it DID service is not
	 * distinguishable here and is recovered by the held-low check or the next timed poll. */
	if (consumed != 0u) {
		od_gpio_irq_lock();
		s_irq_mask = (uint8_t)(s_irq_mask & ~consumed);
		od_gpio_irq_unlock();
	}
}

void opendisplay_touch_resume_after_refresh(void)
{
	const uint32_t now = k_uptime_get_32();
	const uint32_t delay = od_touch_gt911_resume(opendisplay_get_global_config(), now);

	/* OD_TOUCH_NO_CHANGE means the machine was not suspended, or is still nested. Installing a
	 * delay there would postpone a controller that is actively polling. */
	if (delay != OD_TOUCH_NO_CHANGE) {
		s_next_due_ms = now + delay;
	}
}

bool opendisplay_touch_gpio_is_touch_int(uint8_t pin)
{
	return od_touch_gt911_is_int_pin(opendisplay_get_global_config(), pin);
}
