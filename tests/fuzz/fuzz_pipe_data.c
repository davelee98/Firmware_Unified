#include "fuzz_pipe_support.h"

#include "od_pipe.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t cursor;
    uint8_t start_flags;
    od_cmd_ctx_t end_ctx;

    fz_pipe_reset();
    if (size < 4u) {
        return 0;
    }
    start_flags = data[0];
    fz_pipe_open(start_flags, data[1], data[2],
                 (uint16_t)(4u + ((uint16_t)data[3] % (PIPE_MAX_FRAME - 3u))));

    /* A sequence of independently framed DATA writes:
     * [wire_len:2 LE][protected:1][body_len:1][body...]. Keeping one shared machine alive across
     * records reaches reorder drain, duplicate cadence, occupancy growth and sequence wrap. */
    cursor = 4u;
    while (size - cursor >= 4u) {
        const uint16_t wire_len = (uint16_t)(data[cursor]
            | ((uint16_t)data[cursor + 1u] << 8));
        const bool protected_frame = (data[cursor + 2u] & 1u) != 0u;
        size_t body_n = data[cursor + 3u];
        od_cmd_ctx_t ctx;

        cursor += 4u;
        if (body_n > size - cursor) {
            body_n = size - cursor;
        }
        ctx = fz_pipe_ctx(wire_len, protected_frame);
        (void)od_pipe_data(&ctx, od_span_make(data + cursor, body_n));
        cursor += body_n;
    }
    end_ctx = fz_pipe_ctx(2u, false);
    (void)od_pipe_end(&end_ctx, od_span_none());
    fz_pipe_reset();
    return 0;
}
