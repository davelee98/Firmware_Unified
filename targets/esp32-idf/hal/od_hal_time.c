/* od_hal_time for ESP-IDF.
 *
 * Every body here is the shim's implementation moved, not rewritten: compat/arduino_compat.cpp
 * already backed millis()/delay()/delayMicroseconds() with exactly these three IDF calls and
 * exactly this arithmetic. Nothing about timing changes -- the Arduino name is what leaves.
 */

#include "od_hal_time.h"
#include "od_hal_sleep.h"

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

uint32_t od_hal_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void od_hal_delay_ms(uint32_t ms)
{
    /* Round UP, then floor at one tick: see the header. Exact at the current 1000 Hz tick --
     * this is what keeps every caller correct if that ever changes, not what makes them
     * correct today. A delay that rounds to zero is a busy-spin dressed as a wait. */
    TickType_t ticks = (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
    vTaskDelay(ticks ? ticks : 1);
}

void od_hal_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}
