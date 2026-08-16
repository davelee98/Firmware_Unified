/* nordic_session_app_test.c -- targets/nordic-zephyr/src/od_session_app.c, the PRODUCTION file,
 * against fake Zephyr seams.
 *
 * THE DEVICE ID IS WIRE-VISIBLE. Four bytes of the silicon id feed both the KDF and the AUTHENTICATE
 * proof, so which four and in what order is the difference between one device and another to the
 * host: get it wrong and every provisioned tag in a fleet becomes a stranger that no stored key
 * opens. C11 moved that arithmetic out of the pipe file, and a move is exactly the change that can
 * alter it without altering anything a build or a boot would notice.
 *
 * So this pins the packing against hand-computed expectations rather than against the code: the low
 * 32 bits of the 64-bit id, big-endian. It also pins the seam's other half -- that the session is
 * reachable only through od_session_app_state(), and is the same object every time.
 */

#include "od_session_app.h"

#include "od_session.h"
#include "session_fake.h"
#include "zephyr/drivers/hwinfo.h"
#include "zephyr/kernel.h"

#include <stdarg.h>
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

/* --------------------------------------------------------------------------------- the fakes --- */

static uint8_t g_hw_id[8];
static size_t  g_hw_len = 8u;
static int     g_hw_rc;              /* negative to make hwinfo fail */

int hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
    size_t n = (g_hw_len < length) ? g_hw_len : length;

    if (g_hw_rc < 0) {
        return g_hw_rc;
    }
    if (buffer != NULL && n != 0u) {
        memcpy(buffer, g_hw_id, n);
    }
    return (int)n;
}

uint32_t fake_k_uptime_ms;
uint32_t k_uptime_get_32(void) { return fake_k_uptime_ms; }

/* od_session_app.c logs through the target's od_log.h; the sink is here because od_log.c is
 * Zephyr-bound. Nothing asserts on a line -- but the arguments are still compiled, so a wrong
 * conversion specifier is a build error here as it is on the target. */
void _od_log(int level, const char *fmt, ...);
void _od_log(int level, const char *fmt, ...)
{
    va_list ap;
    (void)level; (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}

/* The parsed configuration seam. No case here turns security on -- the config path has its own
 * suites, and what matters here is that the accessor is the one place asked. */
static struct SecurityConfig g_sec_cfg;
static const struct SecurityConfig *g_sec_ptr;

const struct SecurityConfig *od_get_parsed_security(void) { return g_sec_ptr; }

/* ----------------------------------------------------------------------------------- cases --- */

static void set_hw(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                   uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7)
{
    g_hw_id[0] = b0; g_hw_id[1] = b1; g_hw_id[2] = b2; g_hw_id[3] = b3;
    g_hw_id[4] = b4; g_hw_id[5] = b5; g_hw_id[6] = b6; g_hw_id[7] = b7;
    g_hw_len = 8u;
    g_hw_rc = 0;
}

static void test_device_id_packing(void)
{
    uint8_t id[OD_SESSION_DEVICE_ID_LEN];

    /* The eight bytes are folded big-endian into a uint64, and the LOW four are emitted
     * big-endian. So the answer is simply hwinfo bytes 4..7, unchanged and in order -- which is
     * the property to state plainly, because it is what a re-derivation gets wrong. */
    CASE("the four bytes are hwinfo bytes 4..7, in order");
    set_hw(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    od_session_app_device_id(id);
    CHECK(id[0] == 0x44u);
    CHECK(id[1] == 0x55u);
    CHECK(id[2] == 0x66u);
    CHECK(id[3] == 0x77u);

    CASE("the high four bytes cannot influence the answer");
    set_hw(0xFF, 0xFF, 0xFF, 0xFF, 0x44, 0x55, 0x66, 0x77);
    od_session_app_device_id(id);
    CHECK(id[0] == 0x44u && id[1] == 0x55u && id[2] == 0x66u && id[3] == 0x77u);

    CASE("an all-ones id packs to all ones rather than saturating early");
    set_hw(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
    od_session_app_device_id(id);
    CHECK(id[0] == 0xFFu && id[1] == 0xFFu && id[2] == 0xFFu && id[3] == 0xFFu);

    CASE("a zero id is passed through, not rejected -- it is a legitimate silicon value here");
    set_hw(0, 0, 0, 0, 0, 0, 0, 0);
    od_session_app_device_id(id);
    CHECK(id[0] == 0u && id[1] == 0u && id[2] == 0u && id[3] == 0u);

    /* Every byte distinct, so a transposed pair anywhere in the fold shows up. */
    CASE("a byte-distinct id catches a transposition anywhere in the fold");
    set_hw(0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80);
    od_session_app_device_id(id);
    CHECK(id[0] == 0x10u);
    CHECK(id[1] == 0x20u);
    CHECK(id[2] == 0x40u);
    CHECK(id[3] == 0x80u);

    /* hwinfo's status is ignored, deliberately: a device that cannot report an id still has to
     * answer AUTHENTICATE with something rather than refuse to exist. What it must NOT be is
     * stack residue -- the host keys its stored session key on this, so an identity that differs
     * between two boots of the same board is a device that silently stops being the one that was
     * provisioned. The buffer is zeroed, so the answer is deterministic. */
    CASE("a failing hwinfo yields the zero id, not whatever was on the stack");
    set_hw(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22);
    g_hw_rc = -5;
    memset(id, 0x5A, sizeof id);
    od_session_app_device_id(id);
    CHECK(id[0] == 0u && id[1] == 0u && id[2] == 0u && id[3] == 0u);
    g_hw_rc = 0;
}

static void test_state_is_one_object_behind_the_seam(void)
{
    struct od_session *a, *b;

    CASE("the seam hands out the same session every time");
    a = od_session_app_state();
    b = od_session_app_state();
    CHECK(a != NULL);
    CHECK(a == b);

    CASE("it starts cleared, and a clear leaves it that way");
    CHECK(!od_session_authenticated(a));
    od_session_clear(a);
    CHECK(!od_session_authenticated(a));
}

static void test_clock_comes_from_one_place(void)
{
    CASE("the session clock is the kernel uptime, not a second counter");
    fake_k_uptime_ms = 123456u;
    CHECK(od_session_app_now_ms() == 123456u);
    fake_k_uptime_ms = 0xFFFFFFFFu;
    CHECK(od_session_app_now_ms() == 0xFFFFFFFFu);
}

static void test_security_comes_from_the_parsed_config(void)
{
    CASE("no stored config is NULL, which is a protocol state rather than an error");
    g_sec_ptr = NULL;
    CHECK(od_session_app_security() == NULL);

    CASE("and a stored one is handed through unchanged");
    memset(&g_sec_cfg, 0, sizeof g_sec_cfg);
    g_sec_ptr = &g_sec_cfg;
    CHECK(od_session_app_security() == &g_sec_cfg);
}

int main(void)
{
    /* Binds the crypto HAL. od_session_app.c reaches od_session.c, which calls od_hal_crypto_* at
     * link time -- shared/ has no injection seam by design -- so a definition has to come from
     * somewhere, and it is the same fake every other session binary uses rather than a second one
     * that could disagree with it. */
    fake_reset();

    test_device_id_packing();
    test_state_is_one_object_behind_the_seam();
    test_clock_comes_from_one_place();
    test_security_comes_from_the_parsed_config();

    printf("nordic_session_app: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
