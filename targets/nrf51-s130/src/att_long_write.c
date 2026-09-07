#include "od_nrf51_long_write.h"

#include <string.h>

void od_nrf51_long_write_reset(od_nrf51_long_write_t *state)
{
    if (state == NULL) {
        return;
    }
    state->handle = 0u;
    state->length = 0u;
    state->active = false;
}

od_nrf51_lw_result_t od_nrf51_long_write_prepare(
    od_nrf51_long_write_t *state,
    uint16_t expected_handle,
    uint16_t handle,
    uint16_t offset,
    const uint8_t *data,
    uint16_t length)
{
    if (state == NULL || data == NULL || expected_handle == 0u ||
        handle != expected_handle || length == 0u || length > 18u) {
        od_nrf51_long_write_reset(state);
        return OD_NRF51_LW_REJECTED;
    }
    if (!state->active) {
        if (offset != 0u) {
            return OD_NRF51_LW_REJECTED;
        }
        state->active = true;
        state->handle = handle;
    }
    if (state->handle != handle || offset != state->length ||
        length > (uint16_t)(OD_NRF51_MAX_FRAME - state->length)) {
        od_nrf51_long_write_reset(state);
        return OD_NRF51_LW_REJECTED;
    }
    memcpy(state->bytes + state->length, data, length);
    state->length = (uint16_t)(state->length + length);
    return OD_NRF51_LW_ACCEPTED;
}

od_nrf51_lw_result_t od_nrf51_long_write_execute(
    od_nrf51_long_write_t *state,
    bool commit,
    const uint8_t **frame,
    uint16_t *length)
{
    if (frame != NULL) {
        *frame = NULL;
    }
    if (length != NULL) {
        *length = 0u;
    }
    if (state == NULL || frame == NULL || length == NULL || !state->active ||
        state->length == 0u) {
        od_nrf51_long_write_reset(state);
        return OD_NRF51_LW_REJECTED;
    }
    if (!commit) {
        od_nrf51_long_write_reset(state);
        return OD_NRF51_LW_ACCEPTED;
    }
    *frame = state->bytes;
    *length = state->length;
    state->active = false;
    state->handle = 0u;
    state->length = 0u;
    return OD_NRF51_LW_READY;
}
