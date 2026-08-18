#ifndef OD_TEST_FAKE_SILABS_SLEEPTIMER_H
#define OD_TEST_FAKE_SILABS_SLEEPTIMER_H

#include <stdint.h>

uint32_t sl_sleeptimer_get_tick_count(void);
uint32_t sl_sleeptimer_tick_to_ms(uint32_t ticks);

#endif /* OD_TEST_FAKE_SILABS_SLEEPTIMER_H */
