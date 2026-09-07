#include "od_nrf51_config.h"
#include "od_nrf51_long_write.h"
#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"
#include "od_config.h"
#include "od_span.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static uint32_t fake_now;
static uint16_t panel_bytes;
static unsigned panel_begin_calls;
static unsigned panel_refresh_calls;
static unsigned panel_abort_calls;
static int panel_poll_result;
static bool panel_begin_ok = true;
static bool panel_write_ok = true;
static bool panel_refresh_ok = true;

void od_nrf51_copy_msd(uint8_t output[16])
{
    for (uint8_t i = 0u; i < 16u; ++i) {
        output[i] = i;
    }
}

uint32_t od_nrf51_now_ms(void)
{
    return fake_now;
}

bool od_nrf51_panel_begin(void)
{
    panel_begin_calls++;
    return panel_begin_ok;
}

bool od_nrf51_panel_write(const uint8_t *data, uint16_t length)
{
    CHECK(data != NULL);
    panel_bytes = (uint16_t)(panel_bytes + length);
    return panel_write_ok;
}

bool od_nrf51_panel_refresh_start(void)
{
    panel_refresh_calls++;
    return panel_refresh_ok;
}

int od_nrf51_panel_poll(void)
{
    const int result = panel_poll_result;
    panel_poll_result = 0;
    return result;
}

void od_nrf51_panel_abort(void)
{
    panel_abort_calls++;
}

static void reset_fakes(void)
{
    fake_now = 0u;
    panel_bytes = 0u;
    panel_begin_calls = 0u;
    panel_refresh_calls = 0u;
    panel_abort_calls = 0u;
    panel_poll_result = 0;
    panel_begin_ok = true;
    panel_write_ok = true;
    panel_refresh_ok = true;
    od_nrf51_slim_init();
}

static bool expect_tx(const uint8_t *expected, uint8_t expected_length)
{
    const uint8_t *actual = NULL;
    uint8_t actual_length = 0u;
    const bool present = od_nrf51_slim_tx_peek(&actual, &actual_length);

    CHECK(present);
    if (!present) {
        return false;
    }
    CHECK(actual_length == expected_length);
    CHECK(memcmp(actual, expected, expected_length) == 0);
    return actual_length == expected_length &&
           memcmp(actual, expected, expected_length) == 0;
}

static void accept_tx(void)
{
    od_nrf51_slim_tx_accepted();
    od_nrf51_slim_tick();
}

static uint16_t crc_feed(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)((uint16_t)byte << 8);
    for (unsigned bit = 0u; bit < 8u; ++bit) {
        crc = (crc & 0x8000u) != 0u
            ? (uint16_t)(((uint32_t)crc << 1) ^ OD_CONFIG_CRC_POLY)
            : (uint16_t)((uint32_t)crc << 1);
    }
    return crc;
}

static uint16_t config_crc(const uint8_t *blob, uint16_t length)
{
    uint16_t crc = OD_CONFIG_CRC_INIT;

    for (uint16_t i = 0u; i < (uint16_t)(length - 2u); ++i) {
        crc = crc_feed(crc, i < 2u ? 0u : blob[i]);
    }
    return crc;
}

static void test_static_config(void)
{
    uint16_t length = 0u;
    const uint8_t *blob = od_nrf51_config_blob(&length);
    const uint16_t stored_crc = (uint16_t)(blob[length - 2u] |
                                           ((uint16_t)blob[length - 1u] << 8));
    struct od_config parsed;

    CHECK(blob != NULL);
    CHECK(length == 133u);
    CHECK(length <= OD_NRF51_STATIC_CONFIG_MAX_SIZE);
    CHECK(blob[0] == 133u && blob[1] == 0u && blob[2] == OD_CONFIG_VERSION);
    CHECK(blob[3] == 0u && blob[4] == OD_PKT_SYSTEM);
    CHECK(blob[27] == 1u && blob[28] == OD_PKT_MANUFACTURER);
    CHECK(blob[51] == 2u && blob[52] == OD_PKT_POWER);
    CHECK(blob[83] == 3u && blob[84] == OD_PKT_DISPLAY);
    CHECK(od_config_parse(&parsed, od_span_make(blob, length), NULL) == OD_CFG_TLV_OK);
    CHECK(parsed.system_config.ic_type == 0xFFFFu);
    CHECK(parsed.display_count == 1u);
    CHECK(parsed.displays[0].panel_ic_type == OD_PANEL_IC_EP213_104X212);
    CHECK(parsed.displays[0].transmission_modes == OD_TRANSMISSION_MODE_DIRECT_WRITE);
    if (stored_crc != config_crc(blob, length)) {
        fprintf(stderr, "nRF51 config CRC: stored=0x%04x expected=0x%04x\n",
                stored_crc, config_crc(blob, length));
        failures++;
    }
}

static void test_discovery(void)
{
    static const uint8_t version_cmd[] = { 0x00u, 0x43u };
    static const uint8_t msd_cmd[] = { 0x00u, 0x44u };
    static const uint8_t auth_cmd[] = { 0x00u, 0x50u, 0x00u };
    static const uint8_t auth_response[] = { 0x00u, 0x50u, AUTH_STATUS_NOT_CONFIG };
    const uint8_t *tx = NULL;
    uint8_t tx_length = 0u;

    reset_fakes();
    CHECK(od_nrf51_slim_receive(version_cmd, sizeof(version_cmd)));
    CHECK(od_nrf51_slim_tx_peek(&tx, &tx_length));
    CHECK(tx_length == 14u);
    CHECK(tx[0] == RESP_ACK && tx[1] == RESP_FIRMWARE_VERSION);
    CHECK(tx[2] == OD_NRF51_VERSION_MAJOR && tx[3] == OD_NRF51_VERSION_MINOR &&
          tx[4] == 8u && tx[13] == OD_NRF51_VERSION_PATCH);
    CHECK(memcmp(tx + 5u, "00000000", 8u) == 0);
    CHECK(!od_nrf51_slim_receive(msd_cmd, sizeof(msd_cmd)));
    accept_tx();

    CHECK(od_nrf51_slim_receive(msd_cmd, sizeof(msd_cmd)));
    CHECK(od_nrf51_slim_tx_peek(&tx, &tx_length));
    CHECK(tx_length == 18u && tx[0] == 0u && tx[1] == 0x44u);
    for (uint8_t i = 0u; i < 16u; ++i) {
        CHECK(tx[i + 2u] == i);
    }
    accept_tx();

    CHECK(od_nrf51_slim_receive(auth_cmd, sizeof(auth_cmd)));
    (void)expect_tx(auth_response, sizeof(auth_response));
    accept_tx();
}

static void test_config_chunks(void)
{
    static const uint8_t command[] = { 0x00u, 0x40u };
    uint8_t rebuilt[OD_NRF51_STATIC_CONFIG_MAX_SIZE];
    uint16_t rebuilt_length = 0u;
    uint16_t expected_chunk = 0u;
    uint16_t declared_length = 0u;

    reset_fakes();
    CHECK(od_nrf51_slim_receive(command, sizeof(command)));
    CHECK(!od_nrf51_slim_receive(command, sizeof(command)));
    while (!od_nrf51_slim_admission_free()) {
        const uint8_t *tx = NULL;
        uint8_t tx_length = 0u;
        uint8_t header_length = 4u;

        CHECK(od_nrf51_slim_tx_peek(&tx, &tx_length));
        CHECK(tx_length <= OD_NRF51_NOTIFY_MAX);
        CHECK(tx[0] == RESP_ACK && tx[1] == RESP_CONFIG_READ);
        CHECK((uint16_t)(tx[2] | ((uint16_t)tx[3] << 8)) == expected_chunk);
        if (expected_chunk == 0u) {
            declared_length = (uint16_t)(tx[4] | ((uint16_t)tx[5] << 8));
            header_length = 6u;
        }
        memcpy(rebuilt + rebuilt_length, tx + header_length,
               (size_t)(tx_length - header_length));
        rebuilt_length = (uint16_t)(rebuilt_length + tx_length - header_length);
        expected_chunk++;
        accept_tx();
    }
    {
        uint16_t blob_length = 0u;
        const uint8_t *blob = od_nrf51_config_blob(&blob_length);
        CHECK(declared_length == blob_length);
        CHECK(rebuilt_length == blob_length);
        CHECK(memcmp(rebuilt, blob, blob_length) == 0);
        CHECK(expected_chunk == 9u);
    }
}

static void send_and_accept(const uint8_t *frame, uint16_t length, uint8_t echo)
{
    const uint8_t response[] = { RESP_ACK, echo };
    CHECK(od_nrf51_slim_receive(frame, length));
    (void)expect_tx(response, sizeof(response));
    accept_tx();
}

static void test_direct_write(void)
{
    uint8_t data_frame[OD_NRF51_MAX_FRAME] = { 0x00u, 0x71u };
    const uint8_t start[] = { 0x00u, 0x70u };
    const uint8_t end[] = { 0x00u, 0x72u, 0x01u, 1u, 2u, 3u, 4u };
    const uint8_t refresh[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_SUCCESS };
    uint16_t remaining = OD_NRF51_IMAGE_BYTES;

    memset(data_frame + 2u, 0xA5, sizeof(data_frame) - 2u);
    reset_fakes();
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    CHECK(panel_begin_calls == 1u);
    while (remaining != 0u) {
        const uint16_t take = remaining > (OD_NRF51_MAX_FRAME - 2u)
                            ? (OD_NRF51_MAX_FRAME - 2u) : remaining;
        send_and_accept(data_frame, (uint16_t)(take + 2u), RESP_DIRECT_WRITE_DATA_ACK);
        remaining = (uint16_t)(remaining - take);
    }
    CHECK(panel_bytes == OD_NRF51_IMAGE_BYTES);
    CHECK(od_nrf51_slim_receive(end, sizeof(end)));
    CHECK(panel_refresh_calls == 0u);
    {
        const uint8_t ack[] = { RESP_ACK, RESP_DIRECT_WRITE_END_ACK };
        (void)expect_tx(ack, sizeof(ack));
    }
    accept_tx();
    CHECK(panel_refresh_calls == 1u);
    CHECK(!od_nrf51_slim_receive(start, sizeof(start)));
    panel_poll_result = 1;
    od_nrf51_slim_tick();
    (void)expect_tx(refresh, sizeof(refresh));
    accept_tx();
    CHECK(od_nrf51_slim_admission_free());
    CHECK(panel_abort_calls == 0u);

    /* The short END is equally legal, and a refresh-start failure reports 0x74 only after
     * the END ACK has been accepted. */
    reset_fakes();
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    remaining = OD_NRF51_IMAGE_BYTES;
    while (remaining != 0u) {
        const uint16_t take = remaining > (OD_NRF51_MAX_FRAME - 2u)
                            ? (OD_NRF51_MAX_FRAME - 2u) : remaining;
        send_and_accept(data_frame, (uint16_t)(take + 2u), RESP_DIRECT_WRITE_DATA_ACK);
        remaining = (uint16_t)(remaining - take);
    }
    {
        const uint8_t short_end[] = { 0x00u, 0x72u, 0x00u };
        const uint8_t end_ack[] = { RESP_ACK, RESP_DIRECT_WRITE_END_ACK };
        const uint8_t timeout[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_TIMEOUT };
        panel_refresh_ok = false;
        CHECK(od_nrf51_slim_receive(short_end, sizeof(short_end)));
        (void)expect_tx(end_ack, sizeof(end_ack));
        accept_tx();
        (void)expect_tx(timeout, sizeof(timeout));
        accept_tx();
        CHECK(od_nrf51_slim_admission_free());
    }
}

static void test_unsupported(void)
{
    const uint8_t config_write[] = { 0x00u, 0x41u, 1u };
    const uint8_t power_off[] = { 0x00u, 0x52u };
    const uint8_t partial[] = { 0x00u, 0x76u };
    const uint8_t led[] = {
        (uint8_t)(CMD_LED_ACTIVATE >> 8), (uint8_t)CMD_LED_ACTIVATE
    };
    const uint8_t buzzer[] = {
        (uint8_t)(CMD_BUZZER >> 8), (uint8_t)CMD_BUZZER
    };
    const uint8_t pipe[] = { 0x00u, 0x80u };
    const uint8_t unknown[] = { 0x12u, 0x34u };

    reset_fakes();
    CHECK(od_nrf51_slim_receive(config_write, sizeof(config_write)));
    {
        const uint8_t response[] = { RESP_NACK, RESP_CONFIG_WRITE };
        (void)expect_tx(response, sizeof(response));
    }
    accept_tx();
    CHECK(od_nrf51_slim_receive(power_off, sizeof(power_off)));
    {
        const uint8_t response[] = {
            RESP_NACK, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED, 0u
        };
        (void)expect_tx(response, sizeof(response));
    }
    accept_tx();
    CHECK(od_nrf51_slim_receive(partial, sizeof(partial)));
    {
        const uint8_t response[] = {
            RESP_NACK, (uint8_t)CMD_PARTIAL_WRITE_START, OD_ERR_PARTIAL_UNSUPPORTED, 0u
        };
        (void)expect_tx(response, sizeof(response));
    }
    accept_tx();
    CHECK(od_nrf51_slim_receive(led, sizeof(led)));
    {
        const uint8_t response[] = { RESP_NACK, (uint8_t)CMD_LED_ACTIVATE };
        (void)expect_tx(response, sizeof(response));
    }
    accept_tx();
    CHECK(od_nrf51_slim_receive(buzzer, sizeof(buzzer)));
    {
        const uint8_t response[] = { RESP_NACK, (uint8_t)CMD_BUZZER };
        (void)expect_tx(response, sizeof(response));
    }
    accept_tx();
    CHECK(od_nrf51_slim_receive(pipe, sizeof(pipe)));
    {
        const uint8_t response[] = { RESP_NACK, (uint8_t)CMD_PIPE_WRITE_START };
        (void)expect_tx(response, sizeof(response));
    }
    accept_tx();
    CHECK(od_nrf51_slim_receive(unknown, sizeof(unknown)));
    CHECK(od_nrf51_slim_admission_free());
}

static void test_direct_negative_paths(void)
{
    const uint8_t start[] = { 0x00u, 0x70u };
    const uint8_t empty_data[] = { 0x00u, 0x71u };
    const uint8_t short_data[] = { 0x00u, 0x71u, 0xA5u };
    const uint8_t end[] = { 0x00u, 0x72u, 0x00u };
    const uint8_t data_nack[] = { RESP_NACK, RESP_DIRECT_WRITE_DATA_ACK };

    reset_fakes();
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    CHECK(panel_abort_calls == 1u);
    CHECK(panel_begin_calls == 2u);

    reset_fakes();
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    CHECK(od_nrf51_slim_receive(empty_data, sizeof(empty_data)));
    (void)expect_tx(data_nack, sizeof(data_nack));
    accept_tx();
    CHECK(panel_abort_calls == 1u);

    reset_fakes();
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    fake_now = OD_NRF51_TRANSFER_TIMEOUT_MS;
    od_nrf51_slim_tick();
    CHECK(panel_abort_calls == 1u);
    CHECK(od_nrf51_slim_receive(short_data, sizeof(short_data)));
    (void)expect_tx(data_nack, sizeof(data_nack));
    accept_tx();

    reset_fakes();
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    send_and_accept(short_data, sizeof(short_data), RESP_DIRECT_WRITE_DATA_ACK);
    CHECK(od_nrf51_slim_receive(end, sizeof(end)));
    {
        const uint8_t end_nack[] = { RESP_NACK, RESP_DIRECT_WRITE_END_ACK };
        (void)expect_tx(end_nack, sizeof(end_nack));
    }
    accept_tx();
    CHECK(panel_abort_calls == 1u);

    reset_fakes();
    panel_begin_ok = false;
    CHECK(od_nrf51_slim_receive(start, sizeof(start)));
    {
        const uint8_t start_nack[] = { RESP_NACK, RESP_DIRECT_WRITE_START_ACK };
        (void)expect_tx(start_nack, sizeof(start_nack));
    }
    accept_tx();

    reset_fakes();
    send_and_accept(start, sizeof(start), RESP_DIRECT_WRITE_START_ACK);
    panel_write_ok = false;
    CHECK(od_nrf51_slim_receive(short_data, sizeof(short_data)));
    (void)expect_tx(data_nack, sizeof(data_nack));
    accept_tx();
    CHECK(panel_abort_calls == 1u);
}

static void test_long_write(void)
{
    od_nrf51_long_write_t assembler;
    const uint8_t a[] = { 0x00u, 0x71u, 1u, 2u };
    const uint8_t b[] = { 3u, 4u, 5u };
    const uint8_t *frame = NULL;
    uint16_t length = 0u;

    od_nrf51_long_write_reset(&assembler);
    CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 7u, 0u, a, sizeof(a)) ==
          OD_NRF51_LW_ACCEPTED);
    CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 7u, sizeof(a), b, sizeof(b)) ==
          OD_NRF51_LW_ACCEPTED);
    CHECK(od_nrf51_long_write_execute(&assembler, true, &frame, &length) ==
          OD_NRF51_LW_READY);
    CHECK(length == sizeof(a) + sizeof(b));
    CHECK(memcmp(frame, "\x00\x71\x01\x02\x03\x04\x05", length) == 0);

    CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 7u, 1u, a, sizeof(a)) ==
          OD_NRF51_LW_REJECTED);
    CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 7u, 0u, a, sizeof(a)) ==
          OD_NRF51_LW_ACCEPTED);
    CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 7u, 3u, b, sizeof(b)) ==
          OD_NRF51_LW_REJECTED);
    CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 8u, 0u, a, sizeof(a)) ==
          OD_NRF51_LW_REJECTED);
    CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 7u, 0u, a, sizeof(a)) ==
          OD_NRF51_LW_ACCEPTED);
    CHECK(od_nrf51_long_write_execute(&assembler, false, &frame, &length) ==
          OD_NRF51_LW_ACCEPTED);
    CHECK(frame == NULL && length == 0u);

    od_nrf51_long_write_reset(&assembler);
    {
        uint8_t bytes[18] = { 0u };
        uint16_t offset = 0u;
        while (offset < OD_NRF51_MAX_FRAME) {
            const uint16_t take = (uint16_t)(OD_NRF51_MAX_FRAME - offset) > sizeof(bytes)
                                ? sizeof(bytes) : (uint16_t)(OD_NRF51_MAX_FRAME - offset);
            CHECK(od_nrf51_long_write_prepare(&assembler, 7u, 7u, offset,
                                              bytes, take) == OD_NRF51_LW_ACCEPTED);
            offset = (uint16_t)(offset + take);
        }
        CHECK(od_nrf51_long_write_execute(&assembler, true, &frame, &length) ==
              OD_NRF51_LW_READY);
        CHECK(length == OD_NRF51_MAX_FRAME);
    }
}

int main(void)
{
    test_static_config();
    test_discovery();
    test_config_chunks();
    test_direct_write();
    test_direct_negative_paths();
    test_unsupported();
    test_long_write();
    return failures == 0 ? 0 : 1;
}
