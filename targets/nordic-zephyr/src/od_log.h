/*
 * OpenDisplay logging -- the SAME framework as targets/esp32-idf/src/od_log.h, ported to Zephyr.
 *
 * Matched against the ESP32 target IN THIS REPO, not the Arduino original in ../Firmware: that
 * one is already a port (od_log_init() lost its Stream* when the port moved behind od_hal_log),
 * and this header keeps the same signatures so the two targets stay one API.
 *
 * WHY THIS IS NOT ZEPHYR'S LOG SUBSYSTEM. CONFIG_LOG_DEFAULT_LEVEL is the default for EVERY
 * module in the build, so raising it to get more OpenDisplay detail also turns on DEBUG for
 * the kernel and the USB stack: a line per k_mutex_lock/unlock, a line per udc_ep_enqueue.
 * On a CDC-ACM console that is self-feeding -- logging generates USB traffic, which generates
 * log lines. It was tried on the nRF52840 and produced a console of pure os/udc spam with
 * "--- 883 messages dropped ---" burying the app's own output. OD_LOG_LEVEL gates only
 * OpenDisplay's own records, so bench verbosity costs nothing outside this firmware.
 *
 * KEPT IDENTICAL TO THE ESP32 TARGET, deliberately, so one operator reading logs from both
 * targets sees one format and one set of levels:
 *   - the four levels and their numeric order (ERROR=0 .. DEBUG=3), filtered at COMPILE time
 *     so a disabled level costs no flash and no cycles;
 *   - the record shape "[SSSS.mmm|Cn] L: message";
 *   - od_log_raw() for partial lines (progress dots), which must never carry a header;
 *   - od_log_hex_line()'s exact rendering, so a frame dumped by the nRF target and the same
 *     frame dumped by an ESP32 are diffable.
 *
 * WHAT IS DELIBERATELY NOT PORTED, and why each is a non-port rather than an omission:
 *   - the drop accounting and "[DROP: n]" splice. Those exist because the Arduino nRF path
 *     writes through tud_cdc_write() and must reserve TX FIFO space to stay non-blocking.
 *     Zephyr's console API exposes no room query, so porting them would mean counting a
 *     number this target cannot measure. The ESP32 arm never drops or counts either.
 *     od_log_dropped_total() is kept so call sites port unchanged, and honestly returns 0.
 *   - od_log_set_loop_task(). Its argument is a FreeRTOS TaskHandle_t and its job is to give
 *     non-loop callers a zero wait budget. There is no budget here to zero.
 *   - od_log_set_ready_hook(). It suppresses drop-counting on a dark port; with no drop
 *     counting there is nothing for it to suppress.
 */
#ifndef OD_LOG_H
#define OD_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_LOG_ERROR 0
#define OD_LOG_WARN  1
#define OD_LOG_INFO  2
#define OD_LOG_DEBUG 3

#ifndef OD_LOG_LEVEL
#define OD_LOG_LEVEL OD_LOG_INFO
#endif

/*
 * The level test is INSIDE the macro body rather than around it, exactly as in the ESP32
 * source. `if (OD_LOG_LEVEL >= ...)` on a constant is folded away, so a disabled level emits
 * no code -- but its arguments are still COMPILED, so a variable used only for logging never
 * becomes an unused-variable warning at a lower level, and a typo inside a disabled od_log_debug()
 * is still a build error. That property is the reason not to use `#if` here.
 */
#define od_log_error(fmt, ...) do { if (OD_LOG_LEVEL >= OD_LOG_ERROR) \
    _od_log(OD_LOG_ERROR, fmt, ##__VA_ARGS__); } while (0)
#define od_log_warn(fmt, ...)  do { if (OD_LOG_LEVEL >= OD_LOG_WARN) \
    _od_log(OD_LOG_WARN, fmt, ##__VA_ARGS__); } while (0)
#define od_log_info(fmt, ...)  do { if (OD_LOG_LEVEL >= OD_LOG_INFO) \
    _od_log(OD_LOG_INFO, fmt, ##__VA_ARGS__); } while (0)
#define od_log_debug(fmt, ...) do { if (OD_LOG_LEVEL >= OD_LOG_DEBUG) \
    _od_log(OD_LOG_DEBUG, fmt, ##__VA_ARGS__); } while (0)

/*
 * No Stream* parameter: Zephyr's console is bound by devicetree (zephyr,console), not chosen
 * at runtime. Call once early in main(); it only initialises the serialising mutex. Logging
 * before it is called still works -- records simply are not serialised against each other.
 */
void od_log_init(void);

void _od_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Partial line: no header, no newline. For progress output that builds a line piecewise. */
void od_log_raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void od_log_flush(void);

/* Always 0 on this target -- see the header comment. Present for source compatibility. */
uint32_t od_log_dropped_total(void);

/*
 * Builds "<label><space-separated %02X bytes, up to 32><' ...' if truncated>" into buf.
 * Byte-for-byte the same rendering as the ESP32 target's, which is the point: the RX and TX
 * sides of a link must dump a frame identically or the two directions quietly drift apart.
 */
void od_log_hex_line(char *buf, size_t bufSize, const char *label,
                     const uint8_t *data, uint16_t len);

/*
 * The "Cn" field. On ESP32 this is the RTC-persisted deep-sleep wake count; on the Arduino nRF
 * target it is always 0. Weak here and returning 0 for the same reason -- this target has no
 * wake counter yet -- but overridable without touching the logger if one is added.
 */
uint32_t od_log_cycle_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_LOG_H */
