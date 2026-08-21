/* The tag fake's own contract.
 *
 * A fake is normally not worth testing, but this one is load-bearing twice over: every assertion
 * the shared 0x83 suite makes about a refusal rests on its call counters, and its over-cap knob is
 * the only way either adapter's behaviour above the 218-byte cap can be reached from a host test.
 * If the knob were wrong, half of N2b would look covered and be untested.
 */

#include "fake_nfc_tag.h"
#include "od_nfc_app.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)
#define CASE(name) (g_case = (name))

int main(void)
{
    uint8_t buf[512];
    uint8_t type = 0xEEu;
    uint16_t len;

    CASE("a default tag reads four bytes of fill under the requested cap");
    fake_nfc_tag_reset();
    len = 0xFFFFu;
    CHECK(od_nfc_app_read(&type, buf, &len, 218u));
    CHECK(len == 4u);
    CHECK(type == 1u);
    CHECK(buf[0] == 0x5Au && buf[3] == 0x5Au);
    CHECK(fake_nfc_read_calls == 1u);
    CHECK(fake_nfc_read_cap_seen == 218u);

    /* The seam promises len_io is output-only. A caller that left garbage there must still get the
     * true length back, which is what a real adapter does and what this pins. */
    CASE("the in-value of len_io is ignored");
    fake_nfc_tag_reset();
    len = 7u;
    CHECK(od_nfc_app_read(&type, buf, &len, 218u));
    CHECK(len == 4u);

    CASE("a failing tag reports failure and writes nothing");
    fake_nfc_tag_reset();
    fake_nfc_read_ok = false;
    len = 0u;
    CHECK(!od_nfc_app_read(&type, buf, &len, 218u));
    CHECK(fake_nfc_read_calls == 1u);

    CASE("over the cap, a truncating adapter returns exactly the cap");
    fake_nfc_tag_reset();
    fake_nfc_read_len = 219u;
    fake_nfc_over_cap = FAKE_NFC_OVER_CAP_TRUNCATE;
    len = 0u;
    CHECK(od_nfc_app_read(&type, buf, &len, 218u));
    CHECK(len == 218u);

    CASE("over the cap, a refusing adapter returns false");
    fake_nfc_tag_reset();
    fake_nfc_read_len = 219u;
    fake_nfc_over_cap = FAKE_NFC_OVER_CAP_REFUSE;
    len = 0u;
    CHECK(!od_nfc_app_read(&type, buf, &len, 218u));

    CASE("exactly at the cap neither adapter is over it");
    fake_nfc_tag_reset();
    fake_nfc_read_len = 218u;
    fake_nfc_over_cap = FAKE_NFC_OVER_CAP_REFUSE;
    len = 0u;
    CHECK(od_nfc_app_read(&type, buf, &len, 218u));
    CHECK(len == 218u);

    CASE("a write is recorded with its type, length and bytes");
    fake_nfc_tag_reset();
    for (unsigned i = 0; i < 300u; ++i) {
        buf[i] = (uint8_t)(0x40u + i);
    }
    CHECK(od_nfc_app_write(3u, buf, 300u));
    CHECK(fake_nfc_write_calls == 1u);
    CHECK(fake_nfc_write_type == 3u);
    CHECK(fake_nfc_write_len == 300u);
    CHECK(memcmp(fake_nfc_write_data, buf, 300u) == 0);

    CASE("a failing write still records what it was asked to commit");
    fake_nfc_tag_reset();
    fake_nfc_write_ok = false;
    CHECK(!od_nfc_app_write(2u, buf, 8u));
    CHECK(fake_nfc_write_calls == 1u);
    CHECK(fake_nfc_write_len == 8u);

    CASE("reset clears the observations, so one case cannot carry into the next");
    fake_nfc_tag_reset();
    CHECK(fake_nfc_read_calls == 0u);
    CHECK(fake_nfc_write_calls == 0u);
    CHECK(fake_nfc_write_len == 0u);
    CHECK(fake_nfc_write_data[0] == 0u);

    printf("nfc_fake: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
