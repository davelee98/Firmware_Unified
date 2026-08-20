#include "fuzz_pipe_support.h"

#include "od_pipe.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint16_t wire_len = size > 1u ? (uint16_t)(data[0] | ((uint16_t)data[1] << 8)) : 0u;
    bool protected_frame = size > 2u && (data[2] & 1u) != 0u;
    od_cmd_ctx_t ctx = fz_pipe_ctx(wire_len, protected_frame);
    od_span_t body = size > 3u ? od_span_make(data + 3u, size - 3u) : od_span_none();

    fz_pipe_reset();
    fz_pipe_open();
    (void)od_pipe_data(&ctx, body);
    fz_pipe_reset();
    return 0;
}
