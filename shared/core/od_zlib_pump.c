#include "od_zlib_pump.h"

#include "od_inflate_app.h"

static const char s_not_initialized[] = "zlib pump not initialized";
static const char *s_pump_error = s_not_initialized;
static uint32_t s_expected_output;
static uint32_t s_emitted_output;

void od_zlib_pump_reset(uint32_t expected_output_size)
{
    s_pump_error = NULL;
    s_expected_output = expected_output_size;
    s_emitted_output = 0u;
    od_inflate_app_reset(expected_output_size);
}

od_zlib_pump_status_t od_zlib_pump_push(od_span_t input,
                                        bool final,
                                        od_mut_span_t scratch,
                                        od_zlib_sink_fn sink,
                                        void *sink_ctx)
{
    od_zlib_status_t status;

    if (s_pump_error == s_not_initialized) {
        return OD_ZLIB_PUMP_ERROR;
    }
    if (!od_span_valid(input) || !od_mut_span_valid(scratch)
        || scratch.n == 0u || sink == NULL) {
        s_pump_error = "invalid pump argument";
        return OD_ZLIB_PUMP_ERROR;
    }

    status = od_inflate_app_push(input, final);
    if (status == OD_ZLIB_STATUS_ERROR) {
        s_pump_error = od_inflate_app_error();
        return OD_ZLIB_PUMP_ERROR;
    }

    for (;;) {
        size_t produced = 0u;

        status = od_inflate_app_poll(scratch.p, scratch.n, &produced);
        if (produced > scratch.n) {
            s_pump_error = "inflater exceeded output buffer";
            return OD_ZLIB_PUMP_ERROR;
        }
        if (produced > 0u) {
            if (s_emitted_output > s_expected_output
                || produced > (size_t)(s_expected_output - s_emitted_output)) {
                s_pump_error = "decompressed output exceeds expected size";
                return OD_ZLIB_PUMP_ERROR;
            }
            if (!sink(sink_ctx, od_mut_span_make(scratch.p, produced))) {
                s_pump_error = "output sink refused bytes";
                return OD_ZLIB_PUMP_ERROR;
            }
            s_emitted_output += (uint32_t)produced;
        }

        switch (status) {
        case OD_ZLIB_STATUS_OUTPUT_READY:
            break;
        case OD_ZLIB_STATUS_NEEDS_INPUT:
            if (final) {
                s_pump_error = "compressed stream ended before inflater completed";
                return OD_ZLIB_PUMP_ERROR;
            }
            return OD_ZLIB_PUMP_MORE;
        case OD_ZLIB_STATUS_DONE:
            if (s_emitted_output != s_expected_output
                || od_inflate_app_output_count() != s_emitted_output) {
                s_pump_error = "decompressed output size mismatch";
                return OD_ZLIB_PUMP_ERROR;
            }
            return OD_ZLIB_PUMP_DONE;
        case OD_ZLIB_STATUS_ERROR:
            s_pump_error = od_inflate_app_error();
            return OD_ZLIB_PUMP_ERROR;
        }
    }
}

const char *od_zlib_pump_error(void)
{
    return s_pump_error != NULL ? s_pump_error : od_inflate_app_error();
}

uint32_t od_zlib_pump_output_count(void)
{
    return s_emitted_output;
}
