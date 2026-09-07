#include "corpus_runner.h"

#include "od_nrf51_profile.h"
#include "od_nrf51_slim.h"

#include <string.h>

static uint32_t fake_now;
static uint16_t panel_bytes;

unsigned od_corpus_profile_caps(void) { return 0u; }
unsigned od_corpus_profile_mask(void) { return OD_VEC_PROFILE_NRF51; }
bool od_corpus_profile_is_production(void) { return true; }
const char *od_corpus_profile_name(void) { return "nrf51-slim-production"; }

void od_nrf51_copy_msd(uint8_t output[16])
{
    memset(output, 0, 16u);
}

uint32_t od_nrf51_now_ms(void) { return fake_now; }
bool od_nrf51_panel_begin(void) { panel_bytes = 0u; return true; }

bool od_nrf51_panel_write(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length > (uint16_t)(OD_NRF51_IMAGE_BYTES - panel_bytes)) {
        return false;
    }
    panel_bytes = (uint16_t)(panel_bytes + length);
    return true;
}

bool od_nrf51_panel_refresh_start(void)
{
    return panel_bytes == OD_NRF51_IMAGE_BYTES;
}

int od_nrf51_panel_poll(void) { return 1; }
void od_nrf51_panel_abort(void) { panel_bytes = 0u; }

static void discard_tx(void)
{
    const uint8_t *frame;
    uint8_t length;

    while (od_nrf51_slim_tx_peek(&frame, &length)) {
        (void)frame;
        (void)length;
        od_nrf51_slim_tx_accepted();
        od_nrf51_slim_tick();
    }
}

void od_corpus_profile_reset(const od_vec_t *vec)
{
    static const uint8_t start[] = { 0x00u, 0x70u };

    fake_now = 0u;
    panel_bytes = 0u;
    od_nrf51_slim_init();
    if (vec->xfer_active) {
        (void)od_nrf51_slim_receive(start, sizeof(start));
        discard_tx();
    }
}

void od_corpus_profile_dispatch(const uint8_t *frame, uint16_t length)
{
    const uint8_t *reply;
    uint8_t reply_length;
    unsigned limit = 32u;

    (void)od_nrf51_slim_receive(frame, length);
    while (limit-- != 0u && od_nrf51_slim_tx_peek(&reply, &reply_length)) {
        od_corpus_capture(reply, reply_length);
        od_nrf51_slim_tx_accepted();
        od_nrf51_slim_tick();
    }
}
