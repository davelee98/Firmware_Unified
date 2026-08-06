#include "od_pin_codec.h"

#include <stdio.h>

static unsigned checks;
static unsigned failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static void check_absolute(uint8_t encoded, uint8_t want_port, uint8_t want_pin)
{
	uint8_t port = 0xAAu;
	uint8_t pin = 0xAAu;
	CHECK(od_pin_decode_absolute(encoded, 1u, &port, &pin));
	CHECK(port == want_port);
	CHECK(pin == want_pin);
}

static void check_packed(uint8_t encoded, uint8_t want_port, uint8_t want_pin)
{
	uint8_t port = 0xAAu;
	uint8_t pin = 0xAAu;
	CHECK(od_pin_decode_packed(encoded, 3u, &port, &pin));
	CHECK(port == want_port);
	CHECK(pin == want_pin);
}

int main(void)
{
	uint8_t port;
	uint8_t pin;

	check_absolute(15u, 0u, 15u);
	check_absolute(29u, 0u, 29u);
	check_absolute(31u, 0u, 31u);
	check_absolute(44u, 1u, 12u);
	check_absolute(45u, 1u, 13u);
	check_absolute(47u, 1u, 15u);
	CHECK(!od_pin_decode_absolute(0xFFu, 1u, &port, &pin));
	CHECK(!od_pin_decode_absolute(64u, 1u, &port, &pin));

	check_packed(0x23u, 2u, 3u);
	check_packed(0x2Au, 2u, 10u);
	check_packed((uint8_t)(0x80u | (1u << 5) | 31u), 1u, 31u);
	CHECK(!od_pin_decode_packed(0x40u, 3u, &port, &pin));
	CHECK(!od_pin_decode_packed(0xFFu, 3u, &port, &pin));

	printf("nordic pin codec: %u checks, %u failures\n", checks, failures);
	return failures == 0u ? 0 : 1;
}
