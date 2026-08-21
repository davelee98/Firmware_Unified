/* Production BG22 command profile over fake drivers. */

#include "corpus_runner.h"

#include "fake_silabs.h"
#include "od_cmd_test_ctx.h"
#include "od_caps.h"
#include "od_config_asm.h"
#include "od_txq.h"
#include "od_xfer.h"

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
    od_xfer_reset();
    fake_silabs_reset();
    fake_silabs_store_ok = vec->storage_ok != 0u;
    if (vec->xfer_active) {
        static const uint8_t image[4096];
        od_tx_reservation_t reservation;
        od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 9u },
                                            &reservation, 2u, false);
        if (od_txq_reserve(1u, &reservation) == OD_TXQ_OK) {
            (void)od_xfer_direct_start(&ctx, od_span_none());
            od_txq_release(&reservation);
        }
        od_txq_reset();
        if (od_txq_reserve(2u, &reservation) == OD_TXQ_OK) {
            (void)od_xfer_data(&ctx, od_span_make(image, sizeof image));
            od_txq_release(&reservation);
        }
        od_txq_reset();
    }
}
