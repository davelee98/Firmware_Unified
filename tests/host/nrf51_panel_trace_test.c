#include "fake_nrf51_panel.h"
#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    PIN_SCLK = 0,
    PIN_CS = 1,
    PIN_DC = 2,
    PIN_RESET = 3,
    PIN_BUSY = 4,
    PIN_BS = 5,
    PIN_MOSI = 30,
    TRACE_MAX = 6000
};

struct trace_byte {
    uint8_t value;
    bool data;
};

static int failures;
static uint32_t fake_now;
static bool pin_state[32];
static uint32_t output_mask;
static bool busy_state;
static bool configured;
static bool disabled;
static bool hfclk_requested;
static unsigned hfclk_release_calls;
static uint32_t hfclk_request_result;
static uint32_t hfclk_running_result;
static bool hfclk_running;
static struct trace_byte trace[TRACE_MAX];
static unsigned trace_length;
static int fail_spi_at;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

void od_nrf51_panel_test_pin_set(unsigned pin)
{
    CHECK(pin < 32u);
    pin_state[pin] = true;
}

void od_nrf51_panel_test_pin_clear(unsigned pin)
{
    CHECK(pin < 32u);
    pin_state[pin] = false;
}

bool od_nrf51_panel_test_pin_read(unsigned pin)
{
    CHECK(pin == PIN_BUSY);
    return busy_state;
}

void od_nrf51_panel_test_pin_output(unsigned pin)
{
    CHECK(pin < 32u);
    output_mask |= 1u << pin;
}

bool od_nrf51_panel_test_spi(uint8_t value)
{
    const unsigned index = trace_length;

    CHECK(configured);
    CHECK(!pin_state[PIN_CS]);
    CHECK(trace_length < TRACE_MAX);
    if (trace_length < TRACE_MAX) {
        trace[trace_length].value = value;
        trace[trace_length].data = pin_state[PIN_DC];
        trace_length++;
    }
    return fail_spi_at < 0 || index != (unsigned)fail_spi_at;
}

void od_nrf51_panel_test_configure(unsigned sclk, unsigned mosi, unsigned busy)
{
    CHECK(sclk == PIN_SCLK && mosi == PIN_MOSI && busy == PIN_BUSY);
    output_mask |= (1u << sclk) | (1u << mosi);
    configured = true;
    disabled = false;
}

void od_nrf51_panel_test_disable(void)
{
    configured = false;
    disabled = true;
}

void od_nrf51_panel_test_nop(void)
{
    fake_now++;
}

uint32_t od_nrf51_now_ms(void)
{
    return fake_now;
}

uint32_t sd_clock_hfclk_request(void)
{
    hfclk_requested = hfclk_request_result == NRF_SUCCESS;
    return hfclk_request_result;
}

uint32_t sd_clock_hfclk_is_running(uint32_t *running)
{
    *running = hfclk_running ? 1u : 0u;
    return hfclk_running_result;
}

uint32_t sd_clock_hfclk_release(void)
{
    hfclk_requested = false;
    hfclk_release_calls++;
    return NRF_SUCCESS;
}

uint32_t sd_app_evt_wait(void)
{
    fake_now++;
    return NRF_SUCCESS;
}

static void reset_fakes(void)
{
    od_nrf51_panel_abort();
    fake_now = 0u;
    memset(pin_state, 0, sizeof(pin_state));
    output_mask = 0u;
    busy_state = true;
    configured = false;
    disabled = false;
    hfclk_requested = false;
    hfclk_release_calls = 0u;
    hfclk_request_result = NRF_SUCCESS;
    hfclk_running_result = NRF_SUCCESS;
    hfclk_running = true;
    memset(trace, 0, sizeof(trace));
    trace_length = 0u;
    fail_spi_at = -1;
}

static void expected_add(struct trace_byte *expected, unsigned *length,
                         bool is_data, uint8_t value)
{
    expected[*length].value = value;
    expected[*length].data = is_data;
    (*length)++;
}

static void test_full_trace(void)
{
    struct trace_byte expected[TRACE_MAX];
    unsigned expected_length = 0u;
    uint8_t image[OD_NRF51_IMAGE_BYTES];

    memset(image, 0xA5, sizeof(image));
    reset_fakes();
    CHECK(od_nrf51_panel_begin());
    expected_add(expected, &expected_length, false, 0x06u);
    for (unsigned i = 0u; i < 3u; ++i) {
        expected_add(expected, &expected_length, true, 0x17u);
    }
    expected_add(expected, &expected_length, false, 0x04u);
    expected_add(expected, &expected_length, false, 0x71u);
    expected_add(expected, &expected_length, false, 0x00u);
    expected_add(expected, &expected_length, true, 0x13u);
    expected_add(expected, &expected_length, true, 0x0Du);
    expected_add(expected, &expected_length, false, 0x61u);
    expected_add(expected, &expected_length, true, OD_NRF51_WIDTH);
    expected_add(expected, &expected_length, true, 0x00u);
    expected_add(expected, &expected_length, true, OD_NRF51_HEIGHT);
    expected_add(expected, &expected_length, false, 0x50u);
    expected_add(expected, &expected_length, true, 0x97u);
    expected_add(expected, &expected_length, false, 0x10u);
    for (unsigned i = 0u; i < OD_NRF51_IMAGE_BYTES; ++i) {
        expected_add(expected, &expected_length, true, 0xFFu);
    }
    expected_add(expected, &expected_length, false, 0x13u);

    CHECK(od_nrf51_panel_write(image, sizeof(image)));
    for (unsigned i = 0u; i < sizeof(image); ++i) {
        expected_add(expected, &expected_length, true, image[i]);
    }
    CHECK(od_nrf51_panel_refresh_start());
    expected_add(expected, &expected_length, false, 0x12u);
    fake_now += 100u;
    CHECK(od_nrf51_panel_poll() == 0);
    expected_add(expected, &expected_length, false, 0x71u);
    expected_add(expected, &expected_length, false, 0x50u);
    expected_add(expected, &expected_length, true, 0xF7u);
    expected_add(expected, &expected_length, false, 0x02u);
    fake_now += 1u;
    CHECK(od_nrf51_panel_poll() == 1);
    expected_add(expected, &expected_length, false, 0x71u);
    expected_add(expected, &expected_length, false, 0x07u);
    expected_add(expected, &expected_length, true, 0xA5u);

    CHECK(trace_length == expected_length);
    CHECK(memcmp(trace, expected, expected_length * sizeof(expected[0])) == 0);
    CHECK(output_mask == ((1u << PIN_CS) | (1u << PIN_DC) | (1u << PIN_RESET) |
                          (1u << PIN_BS) | (1u << PIN_SCLK) | (1u << PIN_MOSI)));
    CHECK(disabled);
    CHECK(!hfclk_requested && hfclk_release_calls == 1u);
    CHECK(!pin_state[PIN_RESET] && !pin_state[PIN_DC] && pin_state[PIN_CS]);
}

static unsigned successful_begin_length(void)
{
    reset_fakes();
    CHECK(od_nrf51_panel_begin());
    return trace_length;
}

static void test_begin_failures(void)
{
    const unsigned operations = successful_begin_length();

    for (unsigned fail = 0u; fail < operations; ++fail) {
        reset_fakes();
        fail_spi_at = (int)fail;
        CHECK(!od_nrf51_panel_begin());
        CHECK(disabled);
        CHECK(!hfclk_requested && hfclk_release_calls == 1u);
    }

    reset_fakes();
    hfclk_request_result = 1u;
    CHECK(!od_nrf51_panel_begin());
    CHECK(hfclk_release_calls == 0u);
    reset_fakes();
    hfclk_running_result = 1u;
    CHECK(!od_nrf51_panel_begin());
    CHECK(hfclk_release_calls == 1u);
    reset_fakes();
    hfclk_running = false;
    CHECK(!od_nrf51_panel_begin());
    CHECK(fake_now >= 100u && hfclk_release_calls == 1u);
    reset_fakes();
    busy_state = false;
    CHECK(!od_nrf51_panel_begin());
    CHECK(fake_now >= 5000u && disabled);
}

static void begin_and_fill(void)
{
    uint8_t image[OD_NRF51_IMAGE_BYTES];

    memset(image, 0x5Au, sizeof(image));
    CHECK(od_nrf51_panel_begin());
    CHECK(od_nrf51_panel_write(image, sizeof(image)));
}

static void test_transfer_failures_and_total_timeout(void)
{
    uint8_t byte = 0x5Au;

    reset_fakes();
    CHECK(od_nrf51_panel_begin());
    fail_spi_at = (int)trace_length;
    CHECK(!od_nrf51_panel_write(&byte, 1u));
    CHECK(disabled);

    reset_fakes();
    begin_and_fill();
    fail_spi_at = (int)trace_length;
    CHECK(!od_nrf51_panel_refresh_start());
    CHECK(disabled);

    for (unsigned offset = 0u; offset < 4u; ++offset) {
        reset_fakes();
        begin_and_fill();
        CHECK(od_nrf51_panel_refresh_start());
        fake_now += 100u;
        fail_spi_at = (int)(trace_length + offset);
        CHECK(od_nrf51_panel_poll() == -1);
        CHECK(disabled);
    }

    for (unsigned offset = 0u; offset < 3u; ++offset) {
        reset_fakes();
        begin_and_fill();
        CHECK(od_nrf51_panel_refresh_start());
        fake_now += 100u;
        CHECK(od_nrf51_panel_poll() == 0);
        fake_now += 1u;
        fail_spi_at = (int)(trace_length + offset);
        CHECK(od_nrf51_panel_poll() == -1);
        CHECK(disabled);
    }

    reset_fakes();
    begin_and_fill();
    CHECK(od_nrf51_panel_refresh_start());
    fake_now += 100u;
    CHECK(od_nrf51_panel_poll() == 0);
    fake_now += OD_NRF51_PANEL_TIMEOUT_MS - 100u;
    CHECK(od_nrf51_panel_poll() == -1);
    CHECK(disabled);
}

int main(void)
{
    test_full_trace();
    test_begin_failures();
    test_transfer_failures_and_total_timeout();
    return failures == 0 ? 0 : 1;
}
