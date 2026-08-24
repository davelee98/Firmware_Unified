/* ESP32's half of od_sensor_app.h. Three functions, no policy.
 *
 * The SHT40 and BQ27220 drivers are shared/core's now; what stays here is what those drivers
 * cannot do portably -- yield, reach this target's advertisement block, and recover a bus that
 * this target caches.
 */

#include "od_sensor_app.h"

#include "display_service.h"
#include "od_hal_sleep.h"
#include "od_hal_gpio.h"
#include "structs.h"

extern struct od_config globalConfig;

extern uint8_t dynamicreturndata[11];

void od_sensor_app_delay_ms(uint16_t delay_ms)
{
    od_hal_delay_ms(delay_ms);
}

void od_sensor_app_msd_write(uint8_t index, uint8_t value)
{
    if (index < sizeof(dynamicreturndata)) {
        dynamicreturndata[index] = value;
    }
}

void od_sensor_app_bus_recover(uint8_t bus_id)
{
    /* This target keeps ONE live IDF bus, so a retry that does not tear it down is a repeat, not
     * a recovery. Re-open before the settle, not after: the 2 ms is idle-high time on an
     * initialised bus, and where IDF's internal pull-ups are the only pull-ups, waiting with the
     * bus down is not the same wait. */
    invalidateOpenDisplayWire();
    (void)initOrRestoreWireForBus(bus_id);
    od_hal_delay_ms(2);
}

#ifndef OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW
#define OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW (1u << 0)
#endif
#ifndef OD_CHARGER_FLAG_STATE_ACTIVE_LOW
#define OD_CHARGER_FLAG_STATE_ACTIVE_LOW (1u << 1)
#endif

static bool validPin(uint8_t pin) { return pin != 0 && pin != 0xFF; }

bool od_sensor_app_bq_enable(bool on)
{
    const uint8_t en = globalConfig.power_option.charge_enable_pin;
    const bool activeLow =
        (globalConfig.power_option.charger_flags & OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW) != 0;
    const uint8_t st = globalConfig.power_option.charge_state_pin;

    if (validPin(en)) {
        // Configure the output and establish the level in one call -- gpio_config() then
        // gpio_set_level(), the same two register writes in the same order the port used.
        od_hal_gpio_config_output(en, on ? !activeLow : activeLow);
    }
    if (validPin(st)) {
        od_hal_gpio_config_input(st, /*pull_up=*/true, /*pull_down=*/false);
    }
    // An absent enable pin is a successful no-op: plenty of boards have no software charge
    // control, and reporting failure would make a normal board look broken.
    return true;
}

bool od_sensor_app_bq_charging(bool *charging)
{
    const uint8_t st = globalConfig.power_option.charge_state_pin;

    if (!validPin(st) || charging == nullptr) {
        return false;      // no state pin: UNKNOWN, not "not charging"
    }
    const bool activeLow =
        (globalConfig.power_option.charger_flags & OD_CHARGER_FLAG_STATE_ACTIVE_LOW) != 0;
    const int level = od_hal_gpio_read(st);

    *charging = activeLow ? (level == 1) : (level == 0);
    return true;
}
