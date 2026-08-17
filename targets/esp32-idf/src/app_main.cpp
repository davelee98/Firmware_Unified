/* app_main -- the IDF entry point, calling setup()/loop().
 *
 * Under IDF the application starts at app_main(), so the loop that the Arduino core used to
 * run for us lives here explicitly. That makes the scheduling visible and adjustable, which
 * is why the shape stayed after the shim it arrived with was deleted.
 */
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
