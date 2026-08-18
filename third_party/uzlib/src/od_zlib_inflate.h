/*
 * OpenDisplay resumable zlib inflater API.
 *
 * The implementation owns one global stream. Callers reset it with the exact
 * expected output size, push input until final, and poll bounded output chunks.
 */

#ifndef OD_ZLIB_INFLATE_H
#define OD_ZLIB_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OPENDISPLAY_ZLIB_WINDOW_BITS
#define OPENDISPLAY_ZLIB_WINDOW_BITS 9
#endif

#if OPENDISPLAY_ZLIB_WINDOW_BITS < 9 || OPENDISPLAY_ZLIB_WINDOW_BITS > 15
#error "OPENDISPLAY_ZLIB_WINDOW_BITS must be in range 9..15"
#endif

#define OPENDISPLAY_ZLIB_WINDOW_SIZE (1u << OPENDISPLAY_ZLIB_WINDOW_BITS)

typedef enum {
    OD_ZLIB_STATUS_NEEDS_INPUT = 0,
    OD_ZLIB_STATUS_OUTPUT_READY = 1,
    OD_ZLIB_STATUS_DONE = 2,
    OD_ZLIB_STATUS_ERROR = -1,
} od_zlib_status_t;

void od_zlib_stream_reset(uint32_t expected_output_size);
od_zlib_status_t od_zlib_stream_push(const uint8_t *input, size_t len, bool final);
od_zlib_status_t od_zlib_stream_poll(uint8_t *output, size_t capacity, size_t *produced);
const char *od_zlib_stream_error(void);
uint32_t od_zlib_stream_output_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_ZLIB_INFLATE_H */
