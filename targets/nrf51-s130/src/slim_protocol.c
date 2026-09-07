#include "od_nrf51_slim.h"

#include "od_nrf51_config.h"
#include "od_nrf51_profile.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <stddef.h>
#include <string.h>

#ifndef OD_NRF51_BUILD_SHA8
#define OD_NRF51_BUILD_SHA8 "00000000"
#endif

OD_STATIC_ASSERT(sizeof(OD_NRF51_BUILD_SHA8) == 9u,
                 "OD_NRF51_BUILD_SHA8 must contain eight bytes");

typedef enum {
    OD_NRF51_IDLE = 0,
    OD_NRF51_RECEIVING,
    OD_NRF51_WAIT_END_ACK,
    OD_NRF51_REFRESHING,
    OD_NRF51_REPORT_PENDING
} od_nrf51_transfer_state_t;

typedef enum {
    OD_NRF51_PRODUCER_NONE = 0,
    OD_NRF51_PRODUCER_SIMPLE,
    OD_NRF51_PRODUCER_CONFIG,
    OD_NRF51_PRODUCER_END
} od_nrf51_producer_t;

static struct {
    od_nrf51_transfer_state_t transfer;
    od_nrf51_producer_t producer;
    uint32_t deadline_ms;
    uint16_t received;
    uint16_t config_length;
    uint16_t config_offset;
    uint16_t config_chunk;
    const uint8_t *config;
    uint8_t tx[OD_NRF51_NOTIFY_MAX];
    uint8_t tx_length;
    bool tx_valid;
    bool tx_accepted;
} state;

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool queue_frame(const uint8_t *frame, uint8_t length)
{
    if (frame == NULL || length == 0u || length > OD_NRF51_NOTIFY_MAX || state.tx_valid) {
        return false;
    }
    memcpy(state.tx, frame, length);
    state.tx_length = length;
    state.tx_valid = true;
    return true;
}

static bool queue_simple(const uint8_t *frame, uint8_t length)
{
    if (state.producer != OD_NRF51_PRODUCER_NONE) {
        return false;
    }
    state.producer = OD_NRF51_PRODUCER_SIMPLE;
    if (!queue_frame(frame, length)) {
        state.producer = OD_NRF51_PRODUCER_NONE;
        return false;
    }
    return true;
}

static bool queue_ack(uint8_t echo)
{
    const uint8_t response[] = { RESP_ACK, echo };
    return queue_simple(response, (uint8_t)sizeof(response));
}

static bool queue_nack(uint8_t echo)
{
    const uint8_t response[] = { RESP_NACK, echo };
    return queue_simple(response, (uint8_t)sizeof(response));
}

static void abort_transfer(void)
{
    if (state.transfer != OD_NRF51_IDLE) {
        od_nrf51_panel_abort();
    }
    state.transfer = OD_NRF51_IDLE;
    state.received = 0u;
    state.deadline_ms = 0u;
}

static void config_emit(void)
{
    uint8_t frame[OD_NRF51_NOTIFY_MAX];
    uint8_t header = 4u;
    uint16_t remaining;
    uint8_t take;

    if (state.producer != OD_NRF51_PRODUCER_CONFIG || state.tx_valid ||
        state.config == NULL || state.config_offset >= state.config_length) {
        return;
    }
    frame[0] = RESP_ACK;
    frame[1] = RESP_CONFIG_READ;
    frame[2] = (uint8_t)(state.config_chunk & 0xFFu);
    frame[3] = (uint8_t)(state.config_chunk >> 8);
    if (state.config_chunk == 0u) {
        frame[4] = (uint8_t)(state.config_length & 0xFFu);
        frame[5] = (uint8_t)(state.config_length >> 8);
        header = 6u;
    }
    remaining = (uint16_t)(state.config_length - state.config_offset);
    take = (uint8_t)(OD_NRF51_NOTIFY_MAX - header);
    if (remaining < take) {
        take = (uint8_t)remaining;
    }
    memcpy(frame + header, state.config + state.config_offset, take);
    if (queue_frame(frame, (uint8_t)(header + take))) {
        state.config_offset = (uint16_t)(state.config_offset + take);
        state.config_chunk++;
    }
}

void od_nrf51_slim_init(void)
{
    memset(&state, 0, sizeof(state));
}

void od_nrf51_slim_reset(void)
{
    abort_transfer();
    memset(&state, 0, sizeof(state));
}

bool od_nrf51_slim_admission_free(void)
{
    return state.producer == OD_NRF51_PRODUCER_NONE && !state.tx_valid;
}

bool od_nrf51_slim_tx_peek(const uint8_t **frame, uint8_t *length)
{
    if (frame == NULL || length == NULL || !state.tx_valid) {
        return false;
    }
    *frame = state.tx;
    *length = state.tx_length;
    return true;
}

void od_nrf51_slim_tx_accepted(void)
{
    if (!state.tx_valid) {
        return;
    }
    state.tx_valid = false;
    state.tx_length = 0u;
    state.tx_accepted = true;
}

static bool command_config_read(uint16_t body_length)
{
    if (body_length != 0u) {
        return queue_nack(RESP_CONFIG_READ);
    }
    state.config = od_nrf51_config_blob(&state.config_length);
    if (state.config == NULL || state.config_length == 0u ||
        state.config_length > OD_NRF51_STATIC_CONFIG_MAX_SIZE) {
        const uint8_t error[] = { RESP_NACK, RESP_CONFIG_READ, 0u, 0u };
        return queue_simple(error, (uint8_t)sizeof(error));
    }
    state.config_offset = 0u;
    state.config_chunk = 0u;
    state.producer = OD_NRF51_PRODUCER_CONFIG;
    config_emit();
    return true;
}

static bool command_version(uint16_t body_length)
{
    static const char sha[] = OD_NRF51_BUILD_SHA8;
    uint8_t response[14] = {
        RESP_ACK, RESP_FIRMWARE_VERSION,
        OD_NRF51_VERSION_MAJOR, OD_NRF51_VERSION_MINOR, 8u
    };

    if (body_length != 0u) {
        return queue_nack(RESP_FIRMWARE_VERSION);
    }
    memcpy(response + 5u, sha, 8u);
    response[13] = OD_NRF51_VERSION_PATCH;
    return queue_simple(response, (uint8_t)sizeof(response));
}

static bool command_msd(uint16_t body_length)
{
    uint8_t response[18] = { RESP_ACK, RESP_MSD_READ };

    if (body_length != 0u) {
        return queue_nack(RESP_MSD_READ);
    }
    od_nrf51_copy_msd(response + 2u);
    return queue_simple(response, (uint8_t)sizeof(response));
}

static bool command_start(uint16_t body_length)
{
    if (state.transfer != OD_NRF51_IDLE) {
        abort_transfer();
    }
    if (body_length != 0u || !od_nrf51_panel_begin()) {
        return queue_nack(RESP_DIRECT_WRITE_START_ACK);
    }
    state.transfer = OD_NRF51_RECEIVING;
    state.received = 0u;
    state.deadline_ms = od_nrf51_now_ms() + OD_NRF51_TRANSFER_TIMEOUT_MS;
    return queue_ack(RESP_DIRECT_WRITE_START_ACK);
}

static bool command_data(const uint8_t *body, uint16_t body_length)
{
    if (state.transfer != OD_NRF51_RECEIVING || body_length == 0u ||
        body_length > (uint16_t)(OD_NRF51_IMAGE_BYTES - state.received) ||
        !od_nrf51_panel_write(body, body_length)) {
        abort_transfer();
        return queue_nack(RESP_DIRECT_WRITE_DATA_ACK);
    }
    state.received = (uint16_t)(state.received + body_length);
    state.deadline_ms = od_nrf51_now_ms() + OD_NRF51_TRANSFER_TIMEOUT_MS;
    return queue_ack(RESP_DIRECT_WRITE_DATA_ACK);
}

static bool command_end(const uint8_t *body, uint16_t body_length)
{
    const uint8_t ack[] = { RESP_ACK, RESP_DIRECT_WRITE_END_ACK };

    if (state.transfer != OD_NRF51_RECEIVING ||
        (body_length != 1u && body_length != 5u) || body[0] > 1u ||
        state.received != OD_NRF51_IMAGE_BYTES) {
        abort_transfer();
        return queue_nack(RESP_DIRECT_WRITE_END_ACK);
    }
    state.producer = OD_NRF51_PRODUCER_END;
    state.transfer = OD_NRF51_WAIT_END_ACK;
    if (!queue_frame(ack, (uint8_t)sizeof(ack))) {
        abort_transfer();
        state.producer = OD_NRF51_PRODUCER_NONE;
        return false;
    }
    return true;
}

static bool command_unsupported(uint16_t command)
{
    uint8_t response[4] = { RESP_NACK, (uint8_t)command, 0u, 0u };

    switch (command) {
    case CMD_CONFIG_WRITE:
    case CMD_CONFIG_CHUNK:
    case CMD_CONFIG_CLEAR:
        return queue_nack((uint8_t)command);
    case CMD_POWER_OFF:
    case CMD_DEEP_SLEEP:
        return queue_simple(response, (uint8_t)sizeof(response));
    case CMD_PARTIAL_WRITE_START:
        response[2] = OD_ERR_PARTIAL_UNSUPPORTED;
        return queue_simple(response, (uint8_t)sizeof(response));
    case CMD_LED_ACTIVATE:
    case CMD_LED_STOP:
    case CMD_BUZZER:
        return queue_nack((uint8_t)command);
    case CMD_PIPE_WRITE_START:
        return queue_nack((uint8_t)command);
    case CMD_REBOOT:
    case CMD_ENTER_DFU:
    case CMD_PIPE_WRITE_DATA:
    case CMD_PIPE_WRITE_END:
    case CMD_NFC_ENDPOINT:
        return true;
    default:
        return true;
    }
}

bool od_nrf51_slim_receive(const uint8_t *frame, uint16_t length)
{
    uint16_t command;
    const uint8_t *body;
    uint16_t body_length;

    if (frame == NULL || length < 2u || length > OD_NRF51_MAX_FRAME ||
        !od_nrf51_slim_admission_free()) {
        return false;
    }
    command = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
    body = frame + 2u;
    body_length = (uint16_t)(length - 2u);
    switch (command) {
    case CMD_CONFIG_READ:       return command_config_read(body_length);
    case CMD_FIRMWARE_VERSION:  return command_version(body_length);
    case CMD_READ_MSD:          return command_msd(body_length);
    case CMD_AUTHENTICATE: {
        const uint8_t response[] = { RESP_ACK, RESP_AUTHENTICATE, AUTH_STATUS_NOT_CONFIG };
        return queue_simple(response, (uint8_t)sizeof(response));
    }
    case CMD_DIRECT_WRITE_START: return command_start(body_length);
    case CMD_DIRECT_WRITE_DATA:  return command_data(body, body_length);
    case CMD_DIRECT_WRITE_END:   return command_end(body, body_length);
    default:                     return command_unsupported(command);
    }
}

void od_nrf51_slim_tick(void)
{
    if (state.tx_accepted) {
        state.tx_accepted = false;
        if (state.producer == OD_NRF51_PRODUCER_SIMPLE) {
            state.producer = OD_NRF51_PRODUCER_NONE;
        } else if (state.producer == OD_NRF51_PRODUCER_CONFIG) {
            if (state.config_offset >= state.config_length) {
                state.producer = OD_NRF51_PRODUCER_NONE;
                state.config = NULL;
            } else {
                config_emit();
            }
        } else if (state.producer == OD_NRF51_PRODUCER_END) {
            if (state.transfer == OD_NRF51_WAIT_END_ACK) {
                if (od_nrf51_panel_refresh_start()) {
                    state.transfer = OD_NRF51_REFRESHING;
                } else {
                    const uint8_t timeout[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_TIMEOUT };
                    od_nrf51_panel_abort();
                    state.transfer = OD_NRF51_REPORT_PENDING;
                    (void)queue_frame(timeout, (uint8_t)sizeof(timeout));
                }
            } else if (state.transfer == OD_NRF51_REPORT_PENDING) {
                state.transfer = OD_NRF51_IDLE;
                state.producer = OD_NRF51_PRODUCER_NONE;
                state.received = 0u;
            }
        }
    }

    if (state.producer == OD_NRF51_PRODUCER_CONFIG && !state.tx_valid) {
        config_emit();
    }
    if (state.transfer == OD_NRF51_RECEIVING &&
        deadline_reached(od_nrf51_now_ms(), state.deadline_ms)) {
        abort_transfer();
    }
    if (state.transfer == OD_NRF51_REFRESHING && !state.tx_valid) {
        const int result = od_nrf51_panel_poll();
        if (result != 0) {
            uint8_t response[] = {
                RESP_ACK,
                result > 0 ? RESP_DIRECT_WRITE_REFRESH_SUCCESS : RESP_DIRECT_WRITE_REFRESH_TIMEOUT
            };
            if (result < 0) {
                od_nrf51_panel_abort();
            }
            state.transfer = OD_NRF51_REPORT_PENDING;
            (void)queue_frame(response, (uint8_t)sizeof(response));
        }
    }
}
