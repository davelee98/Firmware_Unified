#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "od_zlib_inflate.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Generated independently with Python zlib 1.3.1 over fill_fixture(..., 2048).
 * Byte 2 proves the intended DEFLATE block type: fixed_fixture is BTYPE=1 and
 * dynamic_fixture is BTYPE=2. wide_fixture has a valid 10-bit zlib CMF/FCHECK. */
static const uint8_t fixed_fixture[] = {
    0x18, 0x19, 0x73, 0x74, 0x72, 0x76, 0x71, 0x75, 0x24, 0x9f, 0x08, 0xad, 0x5a, 0x70, 0xf4, 0x95,
    0x80, 0x69, 0x54, 0xc3, 0xd2, 0x53, 0x1f, 0x44, 0xad, 0x12, 0x5a, 0x57, 0x5d, 0xf8, 0x2a, 0xe5,
    0x90, 0xda, 0xb5, 0xe1, 0xea, 0x2f, 0x05, 0xd7, 0xac, 0x09, 0x5b, 0x6f, 0x31, 0x50, 0x66, 0xbc,
    0xab, 0x23, 0x21, 0xf3, 0x55, 0x29, 0x33, 0xde, 0xc9, 0x99, 0x90, 0xf9, 0x5e, 0x94, 0x19, 0xef,
    0xe2, 0x4a, 0xc8, 0xfc, 0x02, 0xca, 0x8c, 0x77, 0x74, 0x22, 0x64, 0xfe, 0x54, 0xca, 0x8c, 0x77,
    0x76, 0x21, 0x64, 0xfe, 0x2e, 0xca, 0x8c, 0x77, 0x75, 0x24, 0x64, 0xfe, 0x03, 0xca, 0x8c, 0x77,
    0x72, 0x26, 0x64, 0x3e, 0x2b, 0x65, 0xc6, 0xbb, 0xb8, 0x12, 0x32, 0x5f, 0x8b, 0x32, 0xe3, 0x1d,
    0x9d, 0x08, 0x99, 0x1f, 0x40, 0x99, 0xf1, 0xce, 0x2e, 0x84, 0xcc, 0x2f, 0xa5, 0xcc, 0x78, 0x57,
    0x47, 0x42, 0xe6, 0xcf, 0xa2, 0xcc, 0x78, 0x27, 0x67, 0x42, 0xe6, 0x1f, 0xa0, 0xcc, 0x78, 0x17,
    0x57, 0x42, 0xe6, 0x3f, 0xa5, 0xcc, 0x78, 0x47, 0x27, 0x42, 0xe6, 0x73, 0x51, 0x66, 0xbc, 0xb3,
    0x0b, 0x21, 0xf3, 0x0d, 0x28, 0x33, 0xde, 0xd5, 0x91, 0x90, 0xf9, 0xa1, 0x94, 0x19, 0xef, 0xe4,
    0x4c, 0xc8, 0xfc, 0x2a, 0xca, 0x8c, 0x77, 0x71, 0x25, 0x64, 0xfe, 0x02, 0xca, 0x8c, 0x77, 0x74,
    0x22, 0x64, 0xfe, 0x51, 0xca, 0x8c, 0x77, 0x76, 0x21, 0x64, 0xfe, 0x2b, 0x24, 0xc5, 0x00, 0xb3,
    0xcb, 0xb7, 0x77,
};

static const uint8_t dynamic_fixture[] = {
    0x18, 0xd3, 0xa5, 0xcf, 0x1d, 0x10, 0x02, 0x01, 0x18, 0x84, 0xe1, 0x24, 0x89, 0x92, 0x24, 0x4a,
    0x92, 0x28, 0x49, 0xa2, 0xbe, 0x3f, 0x89, 0x92, 0x93, 0x28, 0x49, 0xa2, 0x24, 0x39, 0x4a, 0x92,
    0xe8, 0x24, 0x89, 0x4e, 0x92, 0x28, 0x49, 0xa2, 0xe4, 0x24, 0x4a, 0x92, 0x28, 0x49, 0x8e, 0x92,
    0x24, 0x4a, 0xb3, 0x9d, 0x69, 0x65, 0x69, 0xe7, 0x99, 0x79, 0x45, 0xcd, 0x43, 0xfe, 0x9f, 0x24,
    0xcd, 0x8b, 0xb2, 0xde, 0x1b, 0x2f, 0x77, 0x97, 0x57, 0xa3, 0x3f, 0x59, 0xed, 0xaf, 0xef, 0xe6,
    0x60, 0xba, 0x3e, 0xdc, 0x3e, 0xad, 0x98, 0x65, 0xc7, 0x7b, 0x85, 0xe3, 0x43, 0x90, 0xdf, 0xe6,
    0x78, 0x35, 0xe4, 0x0f, 0x95, 0x4c, 0x40, 0xfe, 0xdc, 0xc9, 0x04, 0xe4, 0x6f, 0x84, 0x4c, 0x40,
    0xfe, 0xc9, 0xc8, 0x04, 0xe4, 0x3f, 0x82, 0x4c, 0x40, 0x7e, 0x55, 0xc9, 0x04, 0xe4, 0x77, 0x9c,
    0x4c, 0x40, 0xfe, 0x48, 0xc8, 0x04, 0xe4, 0x2f, 0x8c, 0x4c, 0x40, 0xfe, 0x36, 0xc8, 0x04, 0xe4,
    0x9f, 0x95, 0x4c, 0x40, 0xfe, 0xd3, 0xc9, 0x04, 0xe4, 0xd7, 0x84, 0x4c, 0x40, 0x7e, 0xd7, 0xc8,
    0x04, 0xe4, 0x27, 0x41, 0x26, 0x20, 0x3f, 0x55, 0x32, 0x01, 0xf9, 0xb9, 0x93, 0x09, 0xc8, 0x2f,
    0x84, 0x4c, 0x40, 0x7e, 0xf9, 0x73, 0xfe, 0x02, 0xb3, 0xcb, 0xb7, 0x77,
};

static const uint8_t wide_fixture[] = {
    0x28, 0x91, 0x73, 0x74, 0x72, 0x76, 0x71, 0x75, 0x24, 0x9f, 0x08, 0xad, 0x5a, 0x70, 0xf4, 0x95,
    0x80, 0x69, 0x54, 0xc3, 0xd2, 0x53, 0x1f, 0x44, 0xad, 0x12, 0x5a, 0x57, 0x5d, 0xf8, 0x2a, 0xe5,
    0x90, 0xda, 0xb5, 0xe1, 0xea, 0x2f, 0x05, 0xd7, 0xac, 0x09, 0x5b, 0x6f, 0x31, 0x50, 0x66, 0xbc,
    0xab, 0x23, 0x21, 0xf3, 0x55, 0x29, 0x33, 0xde, 0xc9, 0x99, 0x90, 0xf9, 0x5e, 0x14, 0x86, 0x90,
    0x2b, 0x21, 0xf3, 0x0b, 0x28, 0x0c, 0x21, 0x27, 0x42, 0xe6, 0x4f, 0xa5, 0x30, 0x84, 0x5c, 0x08,
    0x99, 0xbf, 0x8b, 0xd2, 0x48, 0x26, 0x64, 0xfe, 0x03, 0x4a, 0x23, 0x99, 0x90, 0xf9, 0xac, 0x94,
    0x46, 0x32, 0x21, 0xf3, 0xb5, 0x28, 0x8d, 0x64, 0x42, 0xe6, 0x07, 0x50, 0x1a, 0xc9, 0x84, 0xcc,
    0x2f, 0xa5, 0x34, 0x92, 0x09, 0x99, 0x3f, 0x8b, 0xd2, 0x48, 0x26, 0x64, 0xfe, 0x01, 0x4a, 0x23,
    0x99, 0x90, 0xf9, 0x4f, 0x29, 0x8d, 0x64, 0x42, 0xe6, 0x73, 0x51, 0x1a, 0xc9, 0x84, 0xcc, 0x37,
    0xa0, 0x34, 0x92, 0x09, 0x99, 0x1f, 0x4a, 0x69, 0x24, 0x13, 0x32, 0xbf, 0x8a, 0xd2, 0x48, 0x26,
    0x64, 0xfe, 0x02, 0x4a, 0x23, 0x99, 0x90, 0xf9, 0x47, 0x29, 0x8d, 0x64, 0x42, 0xe6, 0xbf, 0x42,
    0x52, 0x0c, 0x00, 0xb3, 0xcb, 0xb7, 0x77,
};

static uint32_t test_adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1u;
    uint32_t b = 0u;
    size_t i;
    for (i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static size_t make_stored_fixture(const uint8_t *src, size_t src_len, uint8_t *out, size_t cap) {
    uint32_t adler;
    uint16_t len;
    uint16_t nlen;
    if (src_len > 65535u || cap < src_len + 11u) return 0;
    len = (uint16_t)src_len;
    nlen = (uint16_t)~len;
    out[0] = 0x18u;
    out[1] = 0x19u;
    out[2] = 0x01u;
    out[3] = (uint8_t)len;
    out[4] = (uint8_t)(len >> 8);
    out[5] = (uint8_t)nlen;
    out[6] = (uint8_t)(nlen >> 8);
    memcpy(out + 7, src, src_len);
    adler = test_adler32(src, src_len);
    out[7 + src_len] = (uint8_t)(adler >> 24);
    out[8 + src_len] = (uint8_t)(adler >> 16);
    out[9 + src_len] = (uint8_t)(adler >> 8);
    out[10 + src_len] = (uint8_t)adler;
    return src_len + 11u;
}

static int output_count_matches(const char *name, size_t emitted) {
    uint32_t count = od_zlib_stream_output_count();
    if (count == emitted) return 1;
    fprintf(stderr,
            "%s: output count mismatch: api=%u emitted=%zu\n",
            name,
            (unsigned int)count,
            emitted);
    return 0;
}

static int run_decode_case(const char *name,
                           const uint8_t *compressed,
                           size_t compressed_len,
                           const uint8_t *expected,
                           size_t expected_len,
                           size_t input_chunk,
                           size_t output_chunk) {
    uint8_t *actual = (uint8_t *)malloc(expected_len != 0u ? expected_len : 1u);
    size_t actual_len = 0;
    size_t pos = 0;
    unsigned int steps = 0;
    bool final_sent = false;
    od_zlib_status_t status = OD_ZLIB_STATUS_NEEDS_INPUT;

    if (actual == NULL) return 0;
    od_zlib_stream_reset((uint32_t)expected_len);

    while (status != OD_ZLIB_STATUS_DONE && steps++ < 1000000u) {
        if (status == OD_ZLIB_STATUS_NEEDS_INPUT) {
            if (pos < compressed_len) {
                size_t n = compressed_len - pos;
                if (n > input_chunk) n = input_chunk;
                status = od_zlib_stream_push(compressed + pos, n, false);
                pos += n;
            } else if (!final_sent) {
                status = od_zlib_stream_push(NULL, 0, true);
                final_sent = true;
            } else {
                fprintf(stderr, "%s: requested input after final\n", name);
                free(actual);
                return 0;
            }
        }

        if (status == OD_ZLIB_STATUS_ERROR) {
            fprintf(stderr, "%s: push error: %s\n", name, od_zlib_stream_error());
            free(actual);
            return 0;
        }

        for (;;) {
            uint8_t outbuf[4096];
            size_t produced = 0;
            size_t cap = output_chunk < sizeof(outbuf) ? output_chunk : sizeof(outbuf);
            status = od_zlib_stream_poll(outbuf, cap, &produced);
            if (actual_len + produced > expected_len) {
                fprintf(stderr, "%s: produced beyond expected buffer\n", name);
                free(actual);
                return 0;
            }
            memcpy(actual + actual_len, outbuf, produced);
            actual_len += produced;
            if (!output_count_matches(name, actual_len)) {
                free(actual);
                return 0;
            }
            if (status != OD_ZLIB_STATUS_OUTPUT_READY) break;
        }

        if (status == OD_ZLIB_STATUS_ERROR) {
            fprintf(stderr, "%s: poll error: %s\n", name, od_zlib_stream_error());
            free(actual);
            return 0;
        }
    }

    if (status != OD_ZLIB_STATUS_DONE) {
        fprintf(stderr, "%s: decoder failed to terminate\n", name);
        free(actual);
        return 0;
    }
    if (actual_len != expected_len || memcmp(actual, expected, expected_len) != 0) {
        fprintf(stderr, "%s: output mismatch (%zu != %zu)\n", name, actual_len, expected_len);
        free(actual);
        return 0;
    }
    free(actual);
    return 1;
}

static int expect_error(const char *name,
                        const uint8_t *compressed,
                        size_t compressed_len,
                        size_t expected_len,
                        size_t input_chunk,
                        const char *expected_error) {
    uint8_t outbuf[7];
    size_t emitted = 0;
    size_t pos = 0;
    unsigned int steps = 0;
    bool final_sent = false;
    od_zlib_status_t status = OD_ZLIB_STATUS_NEEDS_INPUT;

    od_zlib_stream_reset((uint32_t)expected_len);
    while (status != OD_ZLIB_STATUS_ERROR && steps++ < 1000000u) {
        if (status == OD_ZLIB_STATUS_NEEDS_INPUT) {
            if (pos < compressed_len) {
                size_t n = compressed_len - pos;
                if (n > input_chunk) n = input_chunk;
                status = od_zlib_stream_push(compressed + pos, n, false);
                pos += n;
            } else if (!final_sent) {
                status = od_zlib_stream_push(NULL, 0, true);
                final_sent = true;
            } else {
                break;
            }
        }

        if (status != OD_ZLIB_STATUS_ERROR) {
            size_t produced = 0;
            status = od_zlib_stream_poll(outbuf, sizeof(outbuf), &produced);
            emitted += produced;
            if (!output_count_matches(name, emitted)) return 0;
        }
        if (status == OD_ZLIB_STATUS_DONE) break;
    }

    if (status != OD_ZLIB_STATUS_ERROR) {
        fprintf(stderr, "%s: expected error but decoder returned %d\n", name, (int)status);
        return 0;
    }
    if (strcmp(od_zlib_stream_error(), expected_error) != 0) {
        fprintf(stderr,
                "%s: wrong error: got '%s', expected '%s'\n",
                name,
                od_zlib_stream_error(),
                expected_error);
        return 0;
    }
    return output_count_matches(name, emitted);
}

static void fill_fixture(uint8_t *buf, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        if ((i % 97u) < 64u) {
            buf[i] = (uint8_t)('A' + (i % 5u));
        } else {
            buf[i] = (uint8_t)((i * 37u + i / 3u) & 0xffu);
        }
    }
}

int main(void) {
    uint8_t src[2048];
    uint8_t stored_fixture[sizeof(src) + 11u];
    uint8_t corrupt_fixture[sizeof(dynamic_fixture)];
    uint8_t trailing_fixture[sizeof(dynamic_fixture) + 1u];
    size_t stored_fixture_len;
    size_t i;
    int ok = 1;
    const struct {
        const char *name;
        const uint8_t *compressed;
        size_t compressed_len;
    } cases[] = {
        {"stored", stored_fixture, sizeof(stored_fixture)},
        {"fixed", fixed_fixture, sizeof(fixed_fixture)},
        {"dynamic", dynamic_fixture, sizeof(dynamic_fixture)},
    };

    fill_fixture(src, sizeof(src));
    stored_fixture_len = make_stored_fixture(src, sizeof(src), stored_fixture, sizeof(stored_fixture));
    if (stored_fixture_len != sizeof(stored_fixture)) {
        fprintf(stderr, "failed to construct stored fixture\n");
        return 1;
    }
    for (i = 0; i < ARRAY_SIZE(cases); ++i) {
        ok &= run_decode_case(cases[i].name,
                              cases[i].compressed,
                              cases[i].compressed_len,
                              src,
                              sizeof(src),
                              1,
                              1);
        ok &= run_decode_case(cases[i].name,
                              cases[i].compressed,
                              cases[i].compressed_len,
                              src,
                              sizeof(src),
                              23,
                              7);
        ok &= run_decode_case(cases[i].name,
                              cases[i].compressed,
                              cases[i].compressed_len,
                              src,
                              sizeof(src),
                              cases[i].compressed_len,
                              4096);
    }

    ok &= expect_error("oversized-window",
                       wide_fixture,
                       sizeof(wide_fixture),
                       sizeof(src),
                       3,
                       "zlib stream window exceeds firmware limit");
    ok &= expect_error("truncated",
                       dynamic_fixture,
                       sizeof(dynamic_fixture) / 2u,
                       sizeof(src),
                       3,
                       "truncated zlib stream");
    ok &= expect_error("expected-too-small",
                       dynamic_fixture,
                       sizeof(dynamic_fixture),
                       sizeof(src) - 1u,
                       3,
                       "decompressed output exceeds expected size");
    ok &= expect_error("expected-too-large",
                       dynamic_fixture,
                       sizeof(dynamic_fixture),
                       sizeof(src) + 1u,
                       3,
                       "decompressed output size mismatch");

    memcpy(corrupt_fixture, dynamic_fixture, sizeof(corrupt_fixture));
    corrupt_fixture[sizeof(corrupt_fixture) - 1u] ^= 0x55u;
    ok &= expect_error("bad-adler",
                       corrupt_fixture,
                       sizeof(corrupt_fixture),
                       sizeof(src),
                       3,
                       "zlib adler32 mismatch");

    memcpy(trailing_fixture, dynamic_fixture, sizeof(dynamic_fixture));
    trailing_fixture[sizeof(trailing_fixture) - 1u] = 0xaau;
    ok &= expect_error("trailing-input",
                       trailing_fixture,
                       sizeof(trailing_fixture),
                       sizeof(src),
                       sizeof(trailing_fixture),
                       "input after end of zlib stream");

    if (!ok) return 1;
    printf("zlib inflate tests passed\n");
    return 0;
}
