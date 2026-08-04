// Minimal esp_timer shim so src/link_owner.cpp can be compiled and tested on the host.
//
// The reference firmware's link_owner.cpp takes its clock from Arduino's millis(); this
// target's copy reads esp_timer_get_time() instead -- see the OD-PORT note at the top of
// src/link_owner.cpp for why (the Arduino shim is a demolition schedule, and link_owner.cpp is
// the clearest shared/ promotion candidate in the target, where Arduino is forbidden outright).
// So the fake clock has to be reachable through this API.
//
// This is now the ONLY clock shim. There was a sibling hostshim/Arduino.h faking millis() over
// the same od_test_millis variable, kept in step so a case could not advance one clock and not
// the other; it was deleted in phase C step 1 (2026-08-04) once src/od_log.h stopped including
// <Arduino.h> and nothing in the host build included Arduino at all.
//
// Test-only: never on an include path for a firmware build, which gets the real esp_timer from
// ESP-IDF.
#ifndef OD_HOSTSHIM_ESP_TIMER_H
#define OD_HOSTSHIM_ESP_TIMER_H

#include <stdint.h>

// Defined by the test, which drives it directly -- including parking it just below a wrap
// boundary, which is not reachable by waiting. Read atomically because the concurrency cases
// advance it from one thread while the code under test reads it from another; a plain access
// would be a data race in the HARNESS and would mask the race being hunted for in the code.
extern volatile uint32_t od_test_millis;

// Microseconds since boot, as ESP-IDF defines it. Scaling the millisecond clock up by 1000 is
// exact and, crucially, WRAP-PRESERVING for the code under test: link_owner.cpp's od_millis()
// divides by 1000 and truncates to uint32_t, so it recovers od_test_millis bit-for-bit --
// including at the ~49.7-day boundary the wrap cases park on, which is the whole reason the
// test drives the clock directly instead of waiting.
static inline int64_t esp_timer_get_time(void) {
    return (int64_t)__atomic_load_n(&od_test_millis, __ATOMIC_RELAXED) * 1000;
}

#endif  // OD_HOSTSHIM_ESP_TIMER_H
