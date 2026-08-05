/* od_hal_log -- the log byte sink for this target.
 *
 * od_log.c owns the POLICY (timestamps, levels, the drop budget, the TX lock); this owns the
 * PORT. The split is what lets od_log.h stop including <Arduino.h>: the only Arduino thing
 * left in the logger was the `Stream *` it was handed, and a Stream is a port, not policy.
 *
 * Which port is a compile-time board decision, not a runtime one:
 *
 *   OPENDISPLAY_LOG_UART   the console is a real UART (the *-extuart / reTerminal-class
 *                          boards, whose console is an onboard CH343P on GPIO43/44 rather
 *                          than native USB). Opened on the IDF UART driver.
 *   otherwise              stdout, which the IDF console driver routes to whatever
 *                          CONFIG_ESP_CONSOLE_* selects -- USB-CDC on the S3 boards.
 *
 * This is deliberately NOT the shape docs/SHARED_API_DESIGN.md specifies for the eventual
 * shared interface, which is one line sink:
 *
 *     void od_hal_log(const char *line);
 *
 * That narrower contract cannot express the free-space query, and the free-space query is
 * load-bearing on the nRF target (off-loop producers poll it and DISCARD rather than block --
 * see od_log.c's od_port_wait_ready). Narrowing to a line sink is therefore a decision to make
 * when the logger is promoted and both targets are in front of you, not something to force
 * here by writing a contract this target cannot honestly implement. Four functions now, one
 * later, and the extra three all die inside od_log.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the console port. Idempotent. Call once, before od_log_init(). */
void od_hal_log_open(void);

/* False before od_hal_log_open(), or if opening the port failed. od_log treats this as
 * "logging is a no-op", which is also the DISABLE_USB_SERIAL build's steady state. */
bool od_hal_log_is_open(void);

/* Free space in the port's TX buffer, non-blocking. Returns 0 for a closed port -- correct,
 * not defensive: nothing can be written to it, so a caller polling for room should give up
 * rather than spin.
 *
 * The stdout backend reports INT_MAX. That is the honest answer for it and not a placeholder:
 * the IDF console driver exposes no inspectable queue, so writes are bounded by the driver
 * and never by a queue this could measure. Reporting a made-up finite number would turn a
 * caller's backoff into either a permanent stall or a permanent no-op. */
int od_hal_log_room(void);

/* Bytes accepted. Never blocks on host backpressure on either backend: the UART has flow
 * control hardwired off so its FIFO always drains at the baud rate, and stdout is bounded by
 * the console driver. */
size_t od_hal_log_write(const uint8_t *b, size_t n);

/* Pushes queued bytes toward the host. Bounded -- the UART backend waits at most 100 ms for
 * TX to drain. Returns once the DRIVER has the bytes, which is not the same as the host
 * having seen them; od_log_flush() adds the settling pause that covers the difference. */
void od_hal_log_flush(void);

#ifdef __cplusplus
}
#endif
