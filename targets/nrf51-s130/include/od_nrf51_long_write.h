#ifndef OD_NRF51_LONG_WRITE_H
#define OD_NRF51_LONG_WRITE_H

#include "od_nrf51_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OD_NRF51_LW_ACCEPTED = 0,
    OD_NRF51_LW_READY,
    OD_NRF51_LW_REJECTED
} od_nrf51_lw_result_t;

typedef struct {
    uint8_t  bytes[OD_NRF51_MAX_FRAME];
    uint16_t handle;
    uint16_t length;
    bool     active;
} od_nrf51_long_write_t;

void od_nrf51_long_write_reset(od_nrf51_long_write_t *state);

od_nrf51_lw_result_t od_nrf51_long_write_prepare(
    od_nrf51_long_write_t *state,
    uint16_t expected_handle,
    uint16_t handle,
    uint16_t offset,
    const uint8_t *data,
    uint16_t length);

od_nrf51_lw_result_t od_nrf51_long_write_execute(
    od_nrf51_long_write_t *state,
    bool commit,
    const uint8_t **frame,
    uint16_t *length);

#endif
