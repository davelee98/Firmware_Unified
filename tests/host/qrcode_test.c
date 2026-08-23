/* third_party/qrcode — characterization of the vendored encoder.
 *
 * The boot screen already compiles this encoder and hashes rendered output, so it is exercised;
 * what it is not is PINNED. This suite fixes the module bitmap for a spread of inputs so a
 * re-vendor, a "cleanup", or a change of mask heuristic is a failing test rather than a QR code
 * that scans differently on a shipped device.
 *
 * 45 vectors: versions 1..10 crossed with a short payload, a payload at the version's capacity,
 * one a byte under it, and byte mode -- plus the payloads the boot screen and the BG22 renderer
 * actually pass. Capacity boundaries are where an encoder change shows up first.
 *
 * CHARACTERIZATION, NOT A SPECIFICATION. The digests below were taken from this implementation.
 * They assert that behaviour has not changed, not that it is correct against the standard -- a
 * conformance corpus would need reference output this repo does not vendor. If one of these fails
 * after a deliberate encoder change, verify the new output scans before re-baselining it.
 */

#include "qrcode.h"

#include <stdint.h>

#include <string.h>

#include "od_check.h"

/* FNV-1a over the module grid, row-major. Any flipped module changes it. */
static uint32_t digest_modules(QRCode *qr)
{
    uint32_t h = 2166136261u;

    for (uint8_t y = 0; y < qr->size; ++y) {
        for (uint8_t x = 0; x < qr->size; ++x) {
            const uint8_t bit = qrcode_getModule(qr, x, y) ? 1u : 0u;

            h ^= bit;
            h *= 16777619u;
        }
    }
    return h;
}

#define MODULES_MAX 1024   /* qrcode_getBufferSize(10) is well under this */

static uint32_t encode_text(uint8_t version, const char *text, uint8_t *size_out)
{
    static uint8_t modules[MODULES_MAX];
    QRCode qr;

    memset(modules, 0, sizeof modules);
    if (qrcode_getBufferSize(version) > sizeof modules) {
        return 0u;
    }
    if (qrcode_initText(&qr, modules, version, ECC_MEDIUM, text) != 0) {
        return 0u;
    }
    *size_out = qr.size;
    return digest_modules(&qr);
}

/* ------------------------------------------------------------------- buffer contract --- */

static void test_buffer_size_contract(void)
{
    CASE("buffer size grows with version and covers the module grid");
    for (uint8_t v = 1; v <= 10; ++v) {
        const uint16_t need = qrcode_getBufferSize(v);
        const uint16_t side = (uint16_t)(4u * v + 17u);
        const uint16_t bits = (uint16_t)(side * side);

        CHECK(need > 0u);
        CHECK((uint32_t)need * 8u >= bits);           /* one bit per module, at least */
        if (v > 1u) {
            CHECK(need >= qrcode_getBufferSize((uint8_t)(v - 1u)));
        }
    }

    CASE("an out-of-range version reports no buffer rather than a guess");
    CHECK(qrcode_getBufferSize(41u) == 0u);

    CASE("data capacity is monotonic across the supported versions");
    for (uint8_t v = 2; v <= 10; ++v) {
        CHECK(qrcode_getDataCapacityBytes(v) >= qrcode_getDataCapacityBytes((uint8_t)(v - 1u)));
    }
    CHECK(qrcode_getDataCapacityBytes(41u) == 0u);
}

/* --------------------------------------------------------------------- module grid --- */

static void test_module_grid_geometry(void)
{
    static uint8_t modules[MODULES_MAX];
    QRCode qr;

    CASE("size follows the version, and the finder patterns are where they must be");
    for (uint8_t v = 1; v <= 10; ++v) {
        memset(modules, 0, sizeof modules);
        if (qrcode_initText(&qr, modules, v, ECC_MEDIUM, "OD") != 0) {
            continue;                                  /* version too small is a valid refusal */
        }
        CHECK(qr.size == (uint8_t)(4u * v + 17u));
        CHECK(qr.version == v);

        /* Three 7x7 finders: top-left, top-right, bottom-left. Corner module is always dark,
         * and the ring at offset 1 is always light -- true for every version and mask. */
        CHECK(qrcode_getModule(&qr, 0, 0));
        CHECK(qrcode_getModule(&qr, (uint8_t)(qr.size - 1u), 0));
        CHECK(qrcode_getModule(&qr, 0, (uint8_t)(qr.size - 1u)));
        CHECK(!qrcode_getModule(&qr, 1, 1));
        CHECK(!qrcode_getModule(&qr, (uint8_t)(qr.size - 2u), 1));
        CHECK(!qrcode_getModule(&qr, 1, (uint8_t)(qr.size - 2u)));
    }

    CASE("a module query outside the grid is refused rather than read out of bounds");
    memset(modules, 0, sizeof modules);
    CHECK(qrcode_initText(&qr, modules, 6u, ECC_MEDIUM, "OD") == 0);
    CHECK(!qrcode_getModule(&qr, qr.size, 0));
    CHECK(!qrcode_getModule(&qr, 0, qr.size));
}

/* ------------------------------------------------------------------ frozen bitmaps --- */

/* Sentinel payloads: the actual bytes depend on the version's capacity, so the table names them
 * and the runner builds them. Distinct contents so the compiler cannot merge the two addresses. */
static const char OD_FILL_CAP[] = "<fill-at-capacity>";
static const char OD_FILL_UNDER[] = "<fill-under-capacity>";
#define FILL_AT_CAPACITY    OD_FILL_CAP
#define FILL_UNDER_CAPACITY OD_FILL_UNDER
#define URL_OD              "https://example.invalid/od"
#define URL_OE              "https://example.invalid/oe"
#define EMPTY               ""
#define DIGITS40            "0123456789012345678901234567890123456789"

typedef enum { OD_TEXT = 0, OD_BYTES = 1 } qr_mode_t;

/* Versions 1..10 x {short, at capacity, one under capacity, byte mode}, plus the payloads the
 * boot screen and the BG22 renderer actually pass. Every row's digest is this implementation's
 * output at the time of writing -- see the file header on what that does and does not claim. */
static const struct {
    uint8_t version;
    qr_mode_t mode;
    const char *payload;
    uint32_t digest;
    uint8_t size;
} vectors[] = {
    {  1u, OD_TEXT,  "OD",                          0xC1FD8A39u, 21u },
    {  1u, OD_TEXT,  FILL_AT_CAPACITY,              0x7D2ED70Fu, 21u },
    {  1u, OD_TEXT,  FILL_UNDER_CAPACITY,           0xFB7F0821u, 21u },
    {  1u, OD_BYTES, "OD-1",                        0x6864CF47u, 21u },
    {  2u, OD_TEXT,  "OD",                          0x48E97595u, 25u },
    {  2u, OD_TEXT,  FILL_AT_CAPACITY,              0x32C77787u, 25u },
    {  2u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x140ED1CDu, 25u },
    {  2u, OD_BYTES, "OD-1",                        0x8279F673u, 25u },
    {  3u, OD_TEXT,  "OD",                          0x89706524u, 29u },
    {  3u, OD_TEXT,  FILL_AT_CAPACITY,              0x935B736Bu, 29u },
    {  3u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x497B6064u, 29u },
    {  3u, OD_BYTES, "OD-1",                        0xFFAECEBFu, 29u },
    {  4u, OD_TEXT,  "OD",                          0x35FF09F7u, 33u },
    {  4u, OD_TEXT,  FILL_AT_CAPACITY,              0xB147DA8Du, 33u },
    {  4u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x90AF650Bu, 33u },
    {  4u, OD_BYTES, "OD-1",                        0x6B0170B9u, 33u },
    {  5u, OD_TEXT,  "OD",                          0x174473AFu, 37u },
    {  5u, OD_TEXT,  FILL_AT_CAPACITY,              0xDFDC49D5u, 37u },
    {  5u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x7BC8625Bu, 37u },
    {  5u, OD_BYTES, "OD-1",                        0xA75824FDu, 37u },
    {  6u, OD_TEXT,  "OD",                          0x884CC065u, 41u },
    {  6u, OD_TEXT,  FILL_AT_CAPACITY,              0x40C1191Du, 41u },
    {  6u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x60563167u, 41u },
    {  6u, OD_BYTES, "OD-1",                        0x2FAC8567u, 41u },
    {  7u, OD_TEXT,  "OD",                          0xC0824047u, 45u },
    {  7u, OD_TEXT,  FILL_AT_CAPACITY,              0x42C2A317u, 45u },
    {  7u, OD_TEXT,  FILL_UNDER_CAPACITY,           0xFAC29E7Bu, 45u },
    {  7u, OD_BYTES, "OD-1",                        0x67C07A6Du, 45u },
    {  8u, OD_TEXT,  "OD",                          0x3CCBAA8Du, 49u },
    {  8u, OD_TEXT,  FILL_AT_CAPACITY,              0xD430FFAFu, 49u },
    {  8u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x6B33CAABu, 49u },
    {  8u, OD_BYTES, "OD-1",                        0x5D3193E3u, 49u },
    {  9u, OD_TEXT,  "OD",                          0x70BF31DAu, 53u },
    {  9u, OD_TEXT,  FILL_AT_CAPACITY,              0xB9F37059u, 53u },
    {  9u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x8728CF57u, 53u },
    {  9u, OD_BYTES, "OD-1",                        0xC5C316A2u, 53u },
    { 10u, OD_TEXT,  "OD",                          0x0A596B87u, 57u },
    { 10u, OD_TEXT,  FILL_AT_CAPACITY,              0x180B2DA5u, 57u },
    { 10u, OD_TEXT,  FILL_UNDER_CAPACITY,           0x0A59D20Bu, 57u },
    { 10u, OD_BYTES, "OD-1",                        0x155BFF11u, 57u },
    {  6u, OD_TEXT,  URL_OD,                        0x7925B904u, 41u },
    {  6u, OD_TEXT,  URL_OE,                        0x320EEB37u, 41u },
    {  6u, OD_TEXT,  EMPTY,                         0xDA544A97u, 41u },
    {  2u, OD_TEXT,  "OPENDISPLAY",                 0xBC2275CFu, 25u },
    { 10u, OD_TEXT,  DIGITS40,                      0x12F362FFu, 57u },
};

static char g_fill[512];

/* Resolve a sentinel to the bytes it stands for. */
static const char *resolve_payload(const char *payload, uint8_t version)
{
    uint16_t cap;

    if (payload != FILL_AT_CAPACITY && payload != FILL_UNDER_CAPACITY) {
        return payload;
    }
    cap = qrcode_getDataCapacityBytes(version);
    if (payload == FILL_UNDER_CAPACITY && cap > 0u) {
        cap = (uint16_t)(cap - 1u);
    }
    if (cap >= sizeof g_fill) {
        return NULL;                                   /* the fixture, not the encoder, is short */
    }
    memset(g_fill, (payload == FILL_AT_CAPACITY) ? 'A' : 'Z', cap);
    g_fill[cap] = '\0';
    return g_fill;
}

static void test_frozen_output(void)
{
    static uint8_t modules[MODULES_MAX];
    unsigned i;

    CASE("the module bitmap matches the frozen baseline");
    for (i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        const char *payload = resolve_payload(vectors[i].payload, vectors[i].version);
        QRCode qr;
        int8_t rc;

        CHECK(payload != NULL);
        if (payload == NULL) {
            continue;
        }
        memset(modules, 0, sizeof modules);
        if (vectors[i].mode == OD_BYTES) {
            rc = qrcode_initBytes(&qr, modules, vectors[i].version, ECC_MEDIUM,
                                  (uint8_t *)(uintptr_t)payload, (uint16_t)strlen(payload));
        } else {
            rc = qrcode_initText(&qr, modules, vectors[i].version, ECC_MEDIUM, payload);
        }
        CHECK(rc == 0);
        if (rc != 0) {
            continue;
        }
        CHECK(qr.size == vectors[i].size);
        CHECK(digest_modules(&qr) == vectors[i].digest);
    }

    CASE("distinct payloads produce distinct bitmaps");
    {
        uint8_t s1 = 0u, s2 = 0u;
        const uint32_t d1 = encode_text(6u, URL_OD, &s1);
        const uint32_t d2 = encode_text(6u, URL_OE, &s2);

        CHECK(d1 != 0u && d2 != 0u);
        CHECK(d1 != d2);
    }

    CASE("encoding does not depend on buffer history");
    {
        uint8_t sa = 0u, sb = 0u;
        CHECK(encode_text(6u, URL_OD, &sa) == encode_text(6u, URL_OD, &sb));
        CHECK(sa == sb);
    }

    CASE("text and byte modes agree on identical payloads");
    {
        static uint8_t mod_a[MODULES_MAX];
        static uint8_t mod_b[MODULES_MAX];
        static uint8_t payload[] = { 'O', 'D', '-', '1' };
        QRCode qa, qb;

        memset(mod_a, 0, sizeof mod_a);
        memset(mod_b, 0, sizeof mod_b);
        CHECK(qrcode_initText(&qa, mod_a, 4u, ECC_MEDIUM, "OD-1") == 0);
        CHECK(qrcode_initBytes(&qb, mod_b, 4u, ECC_MEDIUM, payload, (uint16_t)sizeof payload) == 0);
        CHECK(qa.size == qb.size);
        CHECK(digest_modules(&qa) == digest_modules(&qb));
    }
}

static void test_capacity_refusal(void)
{
    static uint8_t modules[MODULES_MAX];
    static char big[512];
    QRCode qr;
    const uint16_t cap = qrcode_getDataCapacityBytes(2u);

    CASE("a payload past the version's capacity is refused, not truncated");
    memset(big, 'A', sizeof big);
    big[cap + 1u] = '\0';
    memset(modules, 0, sizeof modules);
    CHECK(qrcode_initText(&qr, modules, 2u, ECC_MEDIUM, big) != 0);
}

int main(void)
{
    test_buffer_size_contract();
    test_module_grid_geometry();
    test_frozen_output();
    test_capacity_refusal();

    return OD_CHECK_REPORT_NONEMPTY("qrcode", 100);
}
