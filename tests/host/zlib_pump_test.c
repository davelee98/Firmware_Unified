#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "od_inflate_app.h"
#include "od_zlib_pump.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

static uint8_t s_output[1600];
static size_t s_output_len;
static size_t s_sink_limit;
static uint32_t s_count_bias;

static const uint8_t backref_fixture[] = {
    0x18, 0xd3, 0x73, 0x74, 0x72, 0x76, 0x71, 0x1c, 0xc5, 0xa3, 0x78, 0x14, 0x8f, 0xe2,
    0x21, 0x80, 0x19, 0x18, 0x99, 0x98, 0x59, 0x58, 0xd9, 0xd8, 0x39, 0x38, 0xb9, 0xb8,
    0x79, 0x78, 0xf9, 0xf8, 0x05, 0x04, 0x85, 0x84, 0x45, 0x44, 0xc5, 0xc4, 0x25, 0x24,
    0xa5, 0xa4, 0x65, 0x64, 0xe5, 0xe4, 0x15, 0x14, 0x95, 0x94, 0x55, 0x54, 0xd5, 0xd4,
    0x35, 0x34, 0xb5, 0xb4, 0x75, 0x74, 0xf5, 0xf4, 0x0d, 0x0c, 0x8d, 0x8c, 0x4d, 0x4c,
    0xcd, 0xcc, 0x2d, 0x2c, 0xad, 0xac, 0x6d, 0x6c, 0xed, 0xec, 0x87, 0xba, 0x7e, 0x00,
    0xa3, 0x5f, 0x57, 0x48,
};

/* The host binds the same seam as a target, using the portable engine underneath. */
void od_inflate_app_reset(uint32_t expected_output_size)
{
    s_count_bias = 0u;
    od_zlib_stream_reset(expected_output_size);
}

od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{
    return od_zlib_stream_push(input.p, input.n, final);
}

od_zlib_status_t od_inflate_app_poll(uint8_t *output, size_t capacity, size_t *produced)
{
    return od_zlib_stream_poll(output, capacity, produced);
}

const char *od_inflate_app_error(void)
{
    return od_zlib_stream_error();
}

uint32_t od_inflate_app_output_count(void)
{
    return od_zlib_stream_output_count() + s_count_bias;
}

static uint32_t adler32(od_span_t bytes)
{
    uint32_t a = 1u;
    uint32_t b = 0u;
    size_t i;

    for (i = 0u; i < bytes.n; ++i) {
        a = (a + bytes.p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static size_t make_stored(od_span_t plain, uint8_t *compressed, size_t capacity)
{
    uint16_t len;
    uint16_t inverse;
    uint32_t checksum;

    if (plain.n > UINT16_MAX || capacity < plain.n + 11u) {
        return 0u;
    }
    len = (uint16_t)plain.n;
    inverse = (uint16_t)~len;
    checksum = adler32(plain);
    compressed[0] = 0x18u;
    compressed[1] = 0x19u;
    compressed[2] = 0x01u;
    compressed[3] = (uint8_t)len;
    compressed[4] = (uint8_t)(len >> 8);
    compressed[5] = (uint8_t)inverse;
    compressed[6] = (uint8_t)(inverse >> 8);
    memcpy(compressed + 7u, plain.p, plain.n);
    compressed[7u + plain.n] = (uint8_t)(checksum >> 24);
    compressed[8u + plain.n] = (uint8_t)(checksum >> 16);
    compressed[9u + plain.n] = (uint8_t)(checksum >> 8);
    compressed[10u + plain.n] = (uint8_t)checksum;
    return plain.n + 11u;
}

static bool collect(void *ctx, od_mut_span_t bytes)
{
    (void)ctx;
    if (bytes.n > s_sink_limit - s_output_len) {
        return false;
    }
    memcpy(s_output + s_output_len, bytes.p, bytes.n);
    s_output_len += bytes.n;
    return true;
}

static void reset_sink(size_t limit)
{
    memset(s_output, 0, sizeof(s_output));
    s_output_len = 0u;
    s_sink_limit = limit;
}

static void make_backref_expected(uint8_t out[1456])
{
    size_t i;

    for (i = 0u; i < 1200u; ++i) out[i] = (uint8_t)"ABCD"[i & 3u];
    for (i = 0u; i < 256u; ++i) out[1200u + i] = (uint8_t)(i & 63u);
}

static int test_reset_is_required(void)
{
    uint8_t scratch_byte;
    od_mut_span_t scratch = od_mut_span_make(&scratch_byte, 1u);

    reset_sink(sizeof(s_output));
    CHECK(od_zlib_pump_push(od_span_none(), false, scratch, collect, NULL)
          == OD_ZLIB_PUMP_ERROR);
    CHECK(strstr(od_zlib_pump_error(), "not initialized") != NULL);
    return 1;
}

static int run_backref(size_t input_chunk, size_t output_capacity)
{
    uint8_t expected[1456];
    uint8_t scratch_bytes[256];
    od_mut_span_t scratch = od_mut_span_make(scratch_bytes, output_capacity);
    size_t pos = 0u;

    make_backref_expected(expected);
    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(sizeof(expected));
    while (pos < sizeof(backref_fixture)) {
        size_t amount = sizeof(backref_fixture) - pos;
        bool final;
        od_zlib_pump_status_t status;

        if (amount > input_chunk) amount = input_chunk;
        final = pos + amount == sizeof(backref_fixture);
        status = od_zlib_pump_push(od_span_make(backref_fixture + pos, amount),
                                   final,
                                   scratch,
                                   collect,
                                   NULL);
        CHECK(status == (final ? OD_ZLIB_PUMP_DONE : OD_ZLIB_PUMP_MORE));
        pos += amount;
    }
    CHECK(s_output_len == sizeof(expected));
    CHECK(memcmp(s_output, expected, sizeof(expected)) == 0);
    return 1;
}

static int test_split_input_and_tiny_scratch(void)
{
    static const uint8_t plain[] = "the shared pump owns no transfer storage";
    uint8_t compressed[sizeof(plain) + 11u];
    uint8_t scratch_byte;
    od_mut_span_t scratch = od_mut_span_make(&scratch_byte, 1u);
    size_t compressed_len = make_stored(od_span_make(plain, sizeof(plain)),
                                        compressed,
                                        sizeof(compressed));
    size_t pos = 0u;

    CHECK(compressed_len != 0u);
    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(sizeof(plain));
    while (pos < compressed_len) {
        size_t amount = compressed_len - pos;
        bool final;
        od_zlib_pump_status_t status;

        if (amount > 3u) amount = 3u;
        final = pos + amount == compressed_len;
        status = od_zlib_pump_push(od_span_make(compressed + pos, amount),
                                   final,
                                   scratch,
                                   collect,
                                   NULL);
        CHECK(status == (final ? OD_ZLIB_PUMP_DONE : OD_ZLIB_PUMP_MORE));
        pos += amount;
    }
    CHECK(s_output_len == sizeof(plain));
    CHECK(memcmp(s_output, plain, sizeof(plain)) == 0);
    CHECK(od_zlib_pump_output_count() == sizeof(plain));
    return 1;
}

static int test_final_truncation_fails(void)
{
    static const uint8_t plain[] = "truncated";
    uint8_t compressed[sizeof(plain) + 11u];
    uint8_t scratch_bytes[4];
    od_mut_span_t scratch = od_mut_span_make(scratch_bytes, sizeof(scratch_bytes));
    size_t compressed_len = make_stored(od_span_make(plain, sizeof(plain)),
                                        compressed,
                                        sizeof(compressed));

    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(sizeof(plain));
    CHECK(od_zlib_pump_push(od_span_make(compressed, compressed_len - 1u),
                            true,
                            scratch,
                            collect,
                            NULL) == OD_ZLIB_PUMP_ERROR);
    CHECK(strstr(od_zlib_pump_error(), "truncated") != NULL);
    return 1;
}

static int test_sink_refusal_fails(void)
{
    static const uint8_t plain[] = "sink refusal";
    uint8_t compressed[sizeof(plain) + 11u];
    uint8_t scratch_bytes[4];
    od_mut_span_t scratch = od_mut_span_make(scratch_bytes, sizeof(scratch_bytes));
    size_t compressed_len = make_stored(od_span_make(plain, sizeof(plain)),
                                        compressed,
                                        sizeof(compressed));

    reset_sink(3u);
    od_zlib_pump_reset(sizeof(plain));
    CHECK(od_zlib_pump_push(od_span_make(compressed, compressed_len),
                            true,
                            scratch,
                            collect,
                            NULL) == OD_ZLIB_PUMP_ERROR);
    CHECK(strstr(od_zlib_pump_error(), "sink") != NULL);
    return 1;
}

static int test_invalid_arguments_fail(void)
{
    uint8_t scratch_byte;
    od_mut_span_t scratch = od_mut_span_make(&scratch_byte, 1u);

    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(0u);
    CHECK(od_zlib_pump_push(od_span_make(NULL, 1u), false, scratch, collect, NULL)
          == OD_ZLIB_PUMP_ERROR);
    od_zlib_pump_reset(0u);
    scratch.p = NULL;
    CHECK(od_zlib_pump_push(od_span_none(), false, scratch, collect, NULL)
          == OD_ZLIB_PUMP_ERROR);
    od_zlib_pump_reset(0u);
    scratch.p = &scratch_byte;
    CHECK(od_zlib_pump_push(od_span_none(), false, scratch, NULL, NULL)
          == OD_ZLIB_PUMP_ERROR);
    return 1;
}

static int test_every_output_capacity(void)
{
    size_t capacity;

    for (capacity = 1u; capacity <= 256u; ++capacity) {
        CHECK(run_backref(7u, capacity));
    }
    return 1;
}

static int test_every_input_split(void)
{
    uint8_t expected[1456];
    uint8_t scratch_bytes[17];
    od_mut_span_t scratch = od_mut_span_make(scratch_bytes, sizeof(scratch_bytes));
    size_t split;

    make_backref_expected(expected);
    for (split = 0u; split <= sizeof(backref_fixture); ++split) {
        od_zlib_pump_status_t status;

        reset_sink(sizeof(s_output));
        od_zlib_pump_reset(sizeof(expected));
        status = od_zlib_pump_push(od_span_make(backref_fixture, split),
                                   false,
                                   scratch,
                                   collect,
                                   NULL);
        CHECK(status == (split == sizeof(backref_fixture)
                         ? OD_ZLIB_PUMP_DONE : OD_ZLIB_PUMP_MORE));
        status = od_zlib_pump_push(od_span_make(backref_fixture + split,
                                                sizeof(backref_fixture) - split),
                                   true,
                                   scratch,
                                   collect,
                                   NULL);
        CHECK(status == OD_ZLIB_PUMP_DONE);
        CHECK(s_output_len == sizeof(expected));
        CHECK(memcmp(s_output, expected, sizeof(expected)) == 0);
    }
    return 1;
}

static int test_size_checksum_reset_and_final_twice(void)
{
    uint8_t expected[1456];
    uint8_t corrupt[sizeof(backref_fixture)];
    uint8_t scratch_bytes[31];
    od_mut_span_t scratch = od_mut_span_make(scratch_bytes, sizeof(scratch_bytes));

    make_backref_expected(expected);
    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(sizeof(expected) - 1u);
    CHECK(od_zlib_pump_push(od_span_make(backref_fixture, sizeof(backref_fixture)),
                            true, scratch, collect, NULL) == OD_ZLIB_PUMP_ERROR);

    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(sizeof(expected) + 1u);
    CHECK(od_zlib_pump_push(od_span_make(backref_fixture, sizeof(backref_fixture)),
                            true, scratch, collect, NULL) == OD_ZLIB_PUMP_ERROR);

    memcpy(corrupt, backref_fixture, sizeof(corrupt));
    corrupt[sizeof(corrupt) - 1u] ^= 1u;
    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(sizeof(expected));
    CHECK(od_zlib_pump_push(od_span_make(corrupt, sizeof(corrupt)),
                            true, scratch, collect, NULL) == OD_ZLIB_PUMP_ERROR);
    CHECK(s_output_len == sizeof(expected));
    CHECK(strstr(od_zlib_pump_error(), "adler32") != NULL);

    /* Reset must recover from every sticky engine/pump failure. */
    CHECK(run_backref(sizeof(backref_fixture), sizeof(scratch_bytes)));
    CHECK(od_zlib_pump_push(od_span_none(), true, scratch, collect, NULL)
          == OD_ZLIB_PUMP_DONE);
    CHECK(s_output_len == sizeof(expected));
    return 1;
}

static int test_backend_count_mismatch_fails(void)
{
    static const uint8_t plain[] = "count mismatch";
    uint8_t compressed[sizeof(plain) + 11u];
    uint8_t scratch_bytes[8];
    od_mut_span_t scratch = od_mut_span_make(scratch_bytes, sizeof(scratch_bytes));
    size_t compressed_len = make_stored(od_span_make(plain, sizeof(plain)),
                                        compressed,
                                        sizeof(compressed));

    reset_sink(sizeof(s_output));
    od_zlib_pump_reset(sizeof(plain));
    s_count_bias = 1u;
    CHECK(od_zlib_pump_push(od_span_make(compressed, compressed_len),
                            true, scratch, collect, NULL) == OD_ZLIB_PUMP_ERROR);
    CHECK(strstr(od_zlib_pump_error(), "size mismatch") != NULL);
    return 1;
}

int main(void)
{
    int passed = 0;

    passed += test_reset_is_required();
    passed += test_split_input_and_tiny_scratch();
    passed += test_final_truncation_fails();
    passed += test_sink_refusal_fails();
    passed += test_invalid_arguments_fail();
    passed += test_every_output_capacity();
    passed += test_every_input_split();
    passed += test_size_checksum_reset_and_final_twice();
    passed += test_backend_count_mismatch_fails();
    if (passed != 9) {
        fprintf(stderr, "zlib pump: %d/9 passed\n", passed);
        return 1;
    }
    printf("zlib pump: 9/9 passed\n");
    return 0;
}
