#include "od_inflate_app.h"

#include "od_inflate_tinfl.h"

extern "C" void od_inflate_app_reset(uint32_t expected_output_size)
{
#if OPENDISPLAY_USE_TINFL
    od_inflate_tinfl_reset(expected_output_size);
#else
    od_zlib_stream_reset(expected_output_size);
#endif
}

extern "C" od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{
#if OPENDISPLAY_USE_TINFL
    return od_inflate_tinfl_push(input.p, input.n, final);
#else
    return od_zlib_stream_push(input.p, input.n, final);
#endif
}

extern "C" od_zlib_status_t od_inflate_app_poll(uint8_t *output,
                                                  size_t capacity,
                                                  size_t *produced)
{
#if OPENDISPLAY_USE_TINFL
    return od_inflate_tinfl_poll(output, capacity, produced);
#else
    return od_zlib_stream_poll(output, capacity, produced);
#endif
}

extern "C" const char *od_inflate_app_error(void)
{
#if OPENDISPLAY_USE_TINFL
    return od_inflate_tinfl_error();
#else
    return od_zlib_stream_error();
#endif
}

extern "C" uint32_t od_inflate_app_output_count(void)
{
#if OPENDISPLAY_USE_TINFL
    return od_inflate_tinfl_output_count();
#else
    return od_zlib_stream_output_count();
#endif
}
