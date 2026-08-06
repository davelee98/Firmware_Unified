/* OpenDisplay application logging carried by Zephyr's native logging transport.
 * OpenDisplay owns the stable "[SSSS.mmm|Cn] L: message" record format and compile-time level
 * filtering. Zephyr owns queueing, serialization, flushing and backend selection. System logs
 * remain visibly distinct and use Zephyr's standard format. */
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

/* Compatibility hook. Zephyr initializes logging and its backends during system startup. */
void od_log_init(void);

void _od_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Native raw record: no prefix or newline. Prefer a normal od_log_* call for complete lines. */
void od_log_raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void od_log_flush(void);

/* Present for source compatibility; native backend drop accounting is not exposed here. */
uint32_t od_log_dropped_total(void);

/*
 * Builds "<label><space-separated %02X bytes, up to 32><' ...' if truncated>" into buf.
 * Byte-for-byte the same rendering as the ESP32 target's, which is the point: the RX and TX
 * sides of a link must dump a frame identically or the two directions quietly drift apart.
 */
void od_log_hex_line(char *buf, size_t bufSize, const char *label,
                     const uint8_t *data, uint16_t len);

/* Retained for source compatibility with the other targets. */
uint32_t od_log_cycle_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_LOG_H */
