/* Production BG22 command profile over fake drivers. */

#include "corpus_runner.h"

#include "fake_silabs.h"
#include "od_caps.h"
#include "od_config_asm.h"
#include "od_txq.h"
#include "opendisplay_display.h"

typedef char silabs_pipe_cap_must_be_zero[(OD_CAP_PIPE == 0) ? 1 : -1];
typedef char silabs_partial_cap_must_be_zero[(OD_CAP_PARTIAL == 0) ? 1 : -1];
typedef char silabs_rxq_cap_must_be_zero[(OD_CAP_RXQ == 0) ? 1 : -1];
typedef char silabs_config_cap_must_be_2048[(OD_CONFIG_MAX_SIZE == 2048u) ? 1 : -1];
typedef char silabs_tx_slots_must_be_three[(OD_TXQ_SLOTS == 3u) ? 1 : -1];

unsigned od_corpus_profile_caps(void)
{
    /* NFC is implemented. PIPE, partial, power latch, buzzer, 4K config and RXQ are not. */
    return OD_VEC_CAP_NFC;
}

bool od_corpus_profile_is_production(void) { return true; }
const char *od_corpus_profile_name(void) { return "silabs-production"; }

void od_corpus_profile_reset(const od_vec_t *vec)
{
    fake_silabs_reset();
    fake_silabs_store_ok = vec->storage_ok != 0u;
    if (vec->xfer_active) {
        (void)opendisplay_display_direct_write_start(NULL, 0u);
    }
}
