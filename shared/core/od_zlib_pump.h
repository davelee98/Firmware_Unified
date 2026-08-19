/* Singleton bounded inflater-to-sink pump. The pump owns stream progression and byte accounting;
 * the selected backend owns its inflater state, while the caller lends scratch storage and a
 * sink. Call od_zlib_pump_reset() before the first push and before starting each new stream. The
 * pump is single-consumer and non-reentrant. */
#ifndef OD_ZLIB_PUMP_H
#define OD_ZLIB_PUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "od_span.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*od_zlib_sink_fn)(void *ctx, od_mut_span_t bytes);

typedef enum {
    OD_ZLIB_PUMP_MORE = 0,
    OD_ZLIB_PUMP_DONE = 1,
    OD_ZLIB_PUMP_ERROR = -1,
} od_zlib_pump_status_t;

void od_zlib_pump_reset(uint32_t expected_output_size);
od_zlib_pump_status_t od_zlib_pump_push(od_span_t input,
                                        bool final,
                                        od_mut_span_t scratch,
                                        od_zlib_sink_fn sink,
                                        void *sink_ctx);
const char *od_zlib_pump_error(void);
uint32_t od_zlib_pump_output_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_ZLIB_PUMP_H */
