/* ESP32's half of od_touch_app.h, and the wiring that keeps the shared machine scheduled.
 *
 * WHAT IS HERE IS DRIVER, NOT POLICY. GPIO, the ISR table, the cached-bus re-selection this
 * target needs and the advertisement write. Cadence, the coordinate map, publish-on-change, the
 * address cascade and the failure backoff are all in shared/core/od_touch_gt911.c.
 */

#include "od_touch_app.h"

#include "od_hal_gpio.h"
#include "od_hal_sleep.h"
#include "od_hal_time.h"
#include "od_touch_gt911.h"
#include "display_service.h"

#include "esp_attr.h"      // IRAM_ATTR

/* Declared here rather than via main.h: that header pulls bb_epaper.h, which is deliberately off
 * the include path and granted per-source in main/CMakeLists.txt. */
extern struct od_config globalConfig;
extern uint8_t dynamicreturndata[11];

/* ONE ISR PER CONTROLLER, because od_hal_gpio_config_irq() takes a bare handler and the index has
 * to come from somewhere. The _arg variant exists and would collapse these four, but it is what
 * the button port wants; keeping touch on the plain form leaves the two independent. */
static volatile uint8_t s_irq_mask = 0;

static void IRAM_ATTR touch_isr_0(void) { s_irq_mask |= 1u << 0; }
static void IRAM_ATTR touch_isr_1(void) { s_irq_mask |= 1u << 1; }
static void IRAM_ATTR touch_isr_2(void) { s_irq_mask |= 1u << 2; }
static void IRAM_ATTR touch_isr_3(void) { s_irq_mask |= 1u << 3; }

static void (*const s_isrs[OD_TOUCH_MAX_CONTROLLERS])(void) = {
    touch_isr_0, touch_isr_1, touch_isr_2, touch_isr_3
};

void od_touch_app_delay_ms(uint16_t ms)
{
    od_hal_delay_ms(ms);
}

void od_touch_app_delay_us(uint32_t us)
{
    od_hal_delay_us(us);
}

void od_touch_app_gpio_set_mode_output(uint8_t pin)
{
    od_hal_gpio_set_mode_output(pin);
}

void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up)
{
    od_hal_gpio_config_input(pin, pull_up, /*pull_down=*/false);
}

void od_touch_app_gpio_write(uint8_t pin, bool level_high)
{
    od_hal_gpio_write(pin, level_high);
}

int od_touch_app_gpio_read(uint8_t pin)
{
    // ASK BEFORE READING: od_hal_gpio_read() returns 0 for a pin it cannot read, so "unreadable"
    // and LOW are the same value there. The driver treats < 0 as "no level", and the charger seam
    // is the standing example of what folding the two together costs (DIVERGENCE_MATRIX 21).
    if (!od_hal_gpio_pin_valid(pin)) {
        return -1;
    }
    return od_hal_gpio_read(pin);
}

bool od_touch_app_gpio_attach_int(uint8_t idx, uint8_t pin)
{
    if (idx >= OD_TOUCH_MAX_CONTROLLERS) {
        return false;
    }
    return od_hal_gpio_config_irq(pin, OD_GPIO_EDGE_FALLING, s_isrs[idx]) == 0;
}

void od_touch_app_gpio_detach_int(uint8_t pin)
{
    od_hal_gpio_clear_irq(pin);
}

bool od_touch_app_bus_prepare(uint8_t bus_id)
{
    // This target caches one live IDF bus, so a transaction for another device's bus has to
    // re-select before it can be trusted. Nordic re-initialises per operation and returns true.
    return initOrRestoreWireForBus(bus_id);
}

void od_touch_app_msd_write(uint8_t index, uint8_t value)
{
    if (index < sizeof(dynamicreturndata)) {
        dynamicreturndata[index] = value;
    }
}

void od_touch_app_msd_publish(void)
{
    updatemsdata();
}

/* ------------------------------------------------------------------ scheduling */

static uint32_t s_next_due_ms = 0;

void initTouchInput(void)
{
    const uint32_t now = od_hal_uptime_ms();

    s_irq_mask = 0;
    s_next_due_ms = now + od_touch_gt911_init(&globalConfig, now);
}

void processTouchInput(void)
{
    uint32_t now;
    uint8_t  mask;
    uint8_t  consumed = 0;

    // Skip while a transfer is streaming: GT911 reads contend with it for the bus AND the loop
    // task. The shared lifecycle query covers direct, PIPE and partial streams. The refresh case
    // is the driver's own suspend/resume, because it has to re-establish the part afterwards.
    if (transferActive()) {
        return;
    }
    now = od_hal_uptime_ms();
    if ((int32_t)(now - s_next_due_ms) < 0) {
        return;
    }

    // Snapshot under the lock, service outside it: the shared machine reports which bits it acted
    // on and this clears exactly those, so an edge that arrives mid-service survives to the next
    // pass instead of being swallowed by a blanket clear.
    od_hal_gpio_irq_lock();
    mask = s_irq_mask;
    od_hal_gpio_irq_unlock();

    s_next_due_ms = now + od_touch_gt911_service(&globalConfig, now, mask, &consumed);

    if (consumed != 0u) {
        od_hal_gpio_irq_lock();
        s_irq_mask = (uint8_t)(s_irq_mask & ~consumed);
        od_hal_gpio_irq_unlock();
    }
}

void touchSuspendForEpdRefresh(void)
{
    od_touch_gt911_suspend();
}

void touchResumeAfterEpdRefresh(void)
{
    const uint32_t now = od_hal_uptime_ms();

    invalidateOpenDisplayWire();
    s_next_due_ms = now + od_touch_gt911_resume(&globalConfig, now);
}

void touchForceResume(void)
{
    const uint32_t now = od_hal_uptime_ms();

    invalidateOpenDisplayWire();
    s_next_due_ms = now + od_touch_gt911_force_resume(&globalConfig, now);
}

bool touch_input_gpio_is_touch_int(uint8_t pin)
{
    return od_touch_gt911_is_int_pin(&globalConfig, pin);
}
