/* app_main -- the IDF entry point, calling the imported Arduino setup()/loop().
 * TEMPORARY; part of the shim and dies with it.
 *
 * The Arduino core supplied this. Under IDF the application starts at app_main(), so the
 * loop lives here explicitly -- which is arguably an improvement, because the scheduling
 * that Arduino hid is now visible and adjustable.
 */
#include "arduino_compat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern void setup();
extern void loop();

extern "C" void app_main(void)
{
    setup();
    for (;;) {
        loop();
        /* Arduino's loop task yields once per iteration so lower-priority work and the idle
         * task run. Without this the watchdog fires on any loop() that never blocks. One
         * tick, not zero: taskYIELD() alone does not feed the idle task on a single core. */
        vTaskDelay(1);
    }
}
