/* corpus_profile_nordic.c -- the Nordic production proof profile.
 *
 * IT DEFINES NO HOOK. That is the whole difference from the portable profile: every
 * `od_cmd_app_*` in this executable comes from targets/nordic-zephyr/src/od_cmd_{device,config,
 * direct,nfc}.c and shared transfer modules, linked as production sources. What is here is the
 * translation from a vector's declared state to the DRIVER knobs underneath them -- a stored blob,
 * a panel return code, a version number.
 *
 * So a green vector in this executable says the firmware emits those bytes. A green vector in the
 * portable one says shared dispatch routed and plumbed it correctly, and no more. The runner
 * reports the two separately because they are not the same claim.
 */

#include "corpus_runner.h"

#include "fake_nordic.h"
#include "od_cmd_test_ctx.h"
#include "od_xfer.h"

#include <string.h>

unsigned od_corpus_profile_caps(void)
{
    /* What this target actually has. Partial, buzzer, PIPE and NFC are all compiled in; there is
     * no D-FF power latch, which is why 0x52 answers the unsupported NACK. A vector whose
     * `forbids` names one of the present ones is excluded here and covered by the portable
     * profile -- and the runner counts that rather than hiding it. */
    return OD_VEC_CAP_PARTIAL | OD_VEC_CAP_BUZZER | OD_VEC_CAP_PIPE | OD_VEC_CAP_NFC |
           OD_VEC_CAP_CONFIG_4K | OD_VEC_CAP_RXQ;
}

/* Every hook here is production Nordic code. */
bool od_corpus_profile_is_production(void) { return true; }

const char *od_corpus_profile_name(void) { return "nordic-production"; }

void od_corpus_profile_reset(const od_vec_t *vec)
{
    od_xfer_reset();
    fake_nordic_reset();

    /* THE VERSION FIELDS ARE THE VECTOR'S, not a constant, because a device really does report
     * whatever it was built as -- and the pre-patch fleet build is a state this profile must be
     * able to enter or the historical vector is unreachable rather than excluded. */
    if (vec->fw_patch_byte) {
        fake_ble_version = 0x0105u;      /* 1.5 */
        fake_ble_patch = 3u;
    } else {
        fake_ble_version = 0x0041u;      /* 0.65, the build the capture came from */
        fake_ble_patch = 0u;
    }

    fake_store_init_ok = (vec->storage_ok != 0u);
    fake_store_save_ok = (vec->storage_ok != 0u);

    /* Arm the real shared machine under the identity the runner will dispatch. Fill it completely:
     * a DATA vector still receives its ordinary trailing-chunk ACK, while an END vector reaches
     * the refresh ordering it exists to prove. */
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
