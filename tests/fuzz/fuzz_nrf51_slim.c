#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t fake_now;
static bool panel_active;
static uint16_t panel_bytes;
static int panel_poll_result;

void od_nrf51_copy_msd(uint8_t output[16])
{
    memset(output, 0, 16u);
}

uint32_t od_nrf51_now_ms(void)
{
    return fake_now;
}

bool od_nrf51_panel_begin(void)
{
    panel_active = true;
    panel_bytes = 0u;
    return true;
}

bool od_nrf51_panel_write(const uint8_t *data, uint16_t length)
{
    if (!panel_active || data == NULL || length > OD_NRF51_IMAGE_BYTES - panel_bytes) {
        return false;
    }
    panel_bytes = (uint16_t)(panel_bytes + length);
    return true;
}

bool od_nrf51_panel_refresh_start(void)
{
    return panel_active && panel_bytes == OD_NRF51_IMAGE_BYTES;
}

int od_nrf51_panel_poll(void)
{
    const int result = panel_poll_result;
    panel_poll_result = 0;
    return result;
}

void od_nrf51_panel_abort(void)
{
    panel_active = false;
    panel_bytes = 0u;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t offset = 0u;

    fake_now = 0u;
    panel_active = false;
    panel_bytes = 0u;
    panel_poll_result = 0;
    od_nrf51_slim_init();

    while (size - offset >= 2u) {
        const uint8_t control = data[offset++];
        const size_t available = size - offset - 1u;
        const uint16_t length = (uint16_t)(data[offset++] %
            ((available < OD_NRF51_MAX_FRAME ? available : OD_NRF51_MAX_FRAME) + 1u));
        const uint8_t *tx = NULL;
        uint8_t tx_length = 0u;

        if ((control & 0x01u) != 0u) {
            od_nrf51_slim_reset();
        }
        panel_poll_result = (control & 0x20u) != 0u ? -1 :
                            ((control & 0x10u) != 0u ? 1 : 0);
        (void)od_nrf51_slim_receive(data + offset, length);
        offset += length;
        if (od_nrf51_slim_tx_peek(&tx, &tx_length)) {
            if (tx == NULL || tx_length == 0u || tx_length > OD_NRF51_NOTIFY_MAX) {
                __builtin_trap();
            }
            if ((control & 0x02u) != 0u) {
                od_nrf51_slim_tx_accepted();
            }
        }
        if ((control & 0x40u) != 0u) {
            fake_now += OD_NRF51_TRANSFER_TIMEOUT_MS;
        }
        od_nrf51_slim_tick();
    }

    for (unsigned drain = 0u; drain < 16u; ++drain) {
        const uint8_t *tx = NULL;
        uint8_t tx_length = 0u;
        if (!od_nrf51_slim_tx_peek(&tx, &tx_length)) {
            break;
        }
        if (tx == NULL || tx_length == 0u || tx_length > OD_NRF51_NOTIFY_MAX) {
            __builtin_trap();
        }
        od_nrf51_slim_tx_accepted();
        od_nrf51_slim_tick();
    }
    od_nrf51_slim_reset();
    return 0;
}
