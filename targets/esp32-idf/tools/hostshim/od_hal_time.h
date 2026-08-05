// Host-test fake for hal/od_hal_time.h.
//
// src/link_owner.cpp takes its clock through od_hal_time (phase C step 5), so the host build
// fakes the HAL rather than the layer underneath it. That is the right level: the test now
// stands exactly where shared/ will stand -- od_hal_* is the only clock interface either is
// allowed to know about -- so a promotion changes nothing here.
//
// This REPLACED tools/hostshim/esp_timer.h, which faked esp_timer_get_time() back when
// link_owner.cpp called it directly, and before that tools/hostshim/Arduino.h, which faked
// millis(). Three fakes over the life of one test, each one following the clock as it moved up
// a layer; only this one is at an interface the code under test is allowed to keep.
//
// Test-only: never on an include path for a firmware build, which gets the real HAL.
#ifndef OD_HOSTSHIM_OD_HAL_TIME_H
#define OD_HOSTSHIM_OD_HAL_TIME_H

#include <stdint.h>

// Defined by the test, which drives it directly -- including parking it just below a wrap
// boundary, which is not reachable by waiting. Read atomically because the concurrency cases
// advance it from one thread while the code under test reads it from another; a plain access
// would be a data race in the HARNESS and would mask the race being hunted for in the code.
extern volatile uint32_t od_test_millis;

static inline uint32_t od_hal_uptime_ms(void) {
    return __atomic_load_n(&od_test_millis, __ATOMIC_RELAXED);
}

// Declared, not defined. link_owner.cpp does not wait -- it is pure arbitration logic, which is
// most of why it is the first promotion candidate -- so a host-side delay that silently did
// nothing would be a trap for whoever adds the first caller. An undefined reference names the
// problem at link time instead.
void od_hal_delay_ms(uint32_t ms);
void od_hal_delay_us(uint32_t us);

#endif  // OD_HOSTSHIM_OD_HAL_TIME_H
