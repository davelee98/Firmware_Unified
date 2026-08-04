// Minimal FreeRTOS shim for the host tests.
//
// src/od_log.h names TaskHandle_t in od_log_set_loop_task()'s declaration -- a declaration the
// host tests never call, so this only has to exist for the header to parse. It used to arrive
// via tools/hostshim/Arduino.h; od_log.h stopped including <Arduino.h> in phase C (the logger
// step, 2026-08-04) and now includes the FreeRTOS headers directly, because FreeRTOS is a real
// dependency of the logger on both targets and Arduino never was.
//
// Test-only: never on an include path for a firmware build, which gets the real headers from
// ESP-IDF.
#ifndef OD_HOSTSHIM_FREERTOS_H
#define OD_HOSTSHIM_FREERTOS_H

typedef void *TaskHandle_t;

#endif  // OD_HOSTSHIM_FREERTOS_H
