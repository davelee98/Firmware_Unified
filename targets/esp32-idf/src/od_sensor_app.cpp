/* ESP32's half of od_sensor_app.h. Three functions, no policy.
 *
 * The SHT40 and BQ27220 drivers are shared/core's now; what stays here is what those drivers
 * cannot do portably -- yield, reach this target's advertisement block, and recover a bus that
 * this target caches.
 */

#include "od_sensor_app.h"

#include "display_service.h"
#include "od_hal_sleep.h"

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
