/* Target-selected streaming inflater used by the shared output pump. */
#ifndef OD_INFLATE_APP_H
#define OD_INFLATE_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "od_span.h"
#include "od_zlib_inflate.h"

#ifdef __cplusplus
extern "C" {
#endif

void od_inflate_app_reset(uint32_t expected_output_size);
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final);
od_zlib_status_t od_inflate_app_poll(uint8_t *output, size_t capacity, size_t *produced);
const char *od_inflate_app_error(void);
uint32_t od_inflate_app_output_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_INFLATE_APP_H */
