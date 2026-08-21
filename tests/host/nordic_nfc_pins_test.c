/* The nRF52840 NFC antenna-pin reservation, against the production pin decoder.
 *
 * WHY THIS EXISTS. P0.09/P0.10 are the NFC antenna pair, and which function owns them is the UICR
 * NFCPINS latch read at reset -- so with NFCT built, no config may hand those pads to a peripheral.
 * The rule is one `if` in od_pin_decode(), and it had no test: the existing pin-codec suite
 * compiles only the generic encoder/decoder, not this platform file.
 *
 * The failure it prevents is silent. gpio_pin_set() on an NFC-owned pad returns success and drives
 * nothing, so without the refusal a config naming P0.09 produces a peripheral that is configured,
 * reports no error, and does not work -- the shape recorded in docs/FOLLOWUPS.md section 8.
 *
 * A config-gated version of this was written and reverted; see the comment in the decoder. This
 * suite pins the static form, because a later "improvement" back to runtime gating would compile
 * and pass every other test in the tree.
 */

#include "od_gpio.h"
#include "od_pin_codec.h"

#include <stdio.h>

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

/* The decoder consults devicetree readiness; on the host every port is ready so the reservation
 * is the only thing that can refuse. */
bool od_gpio_port_ready(uint8_t port)
{
    return port <= 1u;
}

/* Absolute encoding: (port << 5) | pin. */
static uint8_t enc(uint8_t port, uint8_t pin)
{
    return (uint8_t)((port << 5) | pin);
}

int main(void)
{
    uint8_t port = 0xFFu;
    uint8_t pin = 0xFFu;

    CASE("the NFC antenna pair is refused while the tag is built");
    CHECK(!od_pin_decode(enc(0u, 9u), &port, &pin));
    CHECK(!od_pin_decode(enc(0u, 10u), &port, &pin));

    /* THE NEIGHBOURS MUST STILL WORK. A reservation written as a range, or off by one, would take
     * pins the tag does not own -- and nothing else in the tree would notice. */
    CASE("the pins either side of the pair are unaffected");
    CHECK(od_pin_decode(enc(0u, 8u), &port, &pin) && port == 0u && pin == 8u);
    CHECK(od_pin_decode(enc(0u, 11u), &port, &pin) && port == 0u && pin == 11u);

    /* Same pin numbers on port 1 are ordinary GPIO: the pair is P0.09/P0.10, not "pin 9 and 10". */
    CASE("port 1 pins 9 and 10 are ordinary GPIO");
    CHECK(od_pin_decode(enc(1u, 9u), &port, &pin) && port == 1u && pin == 9u);
    CHECK(od_pin_decode(enc(1u, 10u), &port, &pin) && port == 1u && pin == 10u);

    CASE("an unused pin is still refused, and a valid one still decodes");
    CHECK(!od_pin_decode(0xFFu, &port, &pin));
    CHECK(od_pin_decode(enc(0u, 0u), &port, &pin) && port == 0u && pin == 0u);
    CHECK(od_pin_decode(enc(1u, 31u), &port, &pin) && port == 1u && pin == 31u);

    printf("nordic_nfc_pins: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
