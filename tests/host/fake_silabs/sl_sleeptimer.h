#ifndef OD_TEST_FAKE_SILABS_SLEEPTIMER_H
#define OD_TEST_FAKE_SILABS_SLEEPTIMER_H

#include <stdint.h>

#include "sl_status.h"

uint32_t sl_sleeptimer_get_tick_count(void);
uint32_t sl_sleeptimer_tick_to_ms(uint32_t ticks);
uint64_t sl_sleeptimer_get_tick_count64(void);
sl_status_t sl_sleeptimer_tick64_to_ms(uint64_t ticks, uint64_t *ms);

#endif /* OD_TEST_FAKE_SILABS_SLEEPTIMER_H */
