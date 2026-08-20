#include "od_hal_time.h"

#include "sl_sleeptimer.h"
#include "sl_udelay.h"

uint32_t od_hal_uptime_ms(void)
{
  uint64_t ms = 0u;
  sl_status_t sc = sl_sleeptimer_tick64_to_ms(sl_sleeptimer_get_tick_count64(), &ms);

  /* Logging can ask for a timestamp before sl_main_init() starts the sleeptimer. A clock HAL
   * cannot assert or log on that path: return the boot-domain origin until conversion is ready. */
  if (sc != SL_STATUS_OK) {
    return 0u;
  }
  return (uint32_t)ms;
}

void od_hal_delay_us(uint32_t us)
{
  sl_udelay_wait(us);
}
