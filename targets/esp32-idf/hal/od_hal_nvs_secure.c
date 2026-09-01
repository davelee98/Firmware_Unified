/* ESP-IDF implementation of od_hal_nvs_secure_erase. See od_hal_nvs_secure.h for the contract
 * and for why the overwrite is best-effort on a log-structured store. */

#include "od_hal_nvs_secure.h"

#include "od_hal_nvs.h"
#include "od_hal_nvs_esp.h"
#include "od_log.h"

#include <string.h>

#include "nvs.h"
#include "esp_err.h"

int od_hal_nvs_secure_erase(void)
{
    if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
        return OD_HAL_NVS_EIO;
    }

    /* Dropped up front, not per outcome: this function mutates the same NVS entry from a
     * second translation unit, and several of its paths can change the medium and still
     * return an error. One unconditional drop makes a stale cache unreachable without having
     * to be right about which of them did. The cost is one re-read. */
    od_hal_nvs_esp_cache_drop();

    nvs_handle_t h;
    esp_err_t err = nvs_open(OD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return OD_HAL_NVS_OK;   /* nothing stored */
    }
    if (err != ESP_OK) {
        return OD_HAL_NVS_EIO;
    }

    size_t stored = 0;
    err = nvs_get_blob(h, OD_NVS_KEY, NULL, &stored);
    if (err == ESP_OK && stored > 0) {
        /* Chunked so the wipe never needs a buffer the size of the record. */
        uint8_t zeros[64];
        memset(zeros, 0, sizeof(zeros));
        if (stored <= sizeof(zeros)) {
            (void)nvs_set_blob(h, OD_NVS_KEY, zeros, stored);
        } else {
            /* NVS has no partial-blob write, so the zero record is built in one pass. This
             * runs once, off the transfer path. */
            static uint8_t zero_blob[OD_HAL_NVS_MAX_RECORD];
            if (stored <= sizeof(zero_blob)) {
                memset(zero_blob, 0, stored);
                (void)nvs_set_blob(h, OD_NVS_KEY, zero_blob, stored);
            }
        }
        (void)nvs_commit(h);
    }

    err = nvs_erase_key(h, OD_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        return OD_HAL_NVS_EIO;
    }

    od_log_info("config record zero-written and erased (%u B)", (unsigned)stored);
    return OD_HAL_NVS_OK;
}
