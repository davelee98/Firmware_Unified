/* ESP-IDF NVS implementation of od_hal_nvs. See shared/hal/od_hal_nvs.h for the contract. */

#include "od_hal_nvs.h"
#include "od_hal_nvs_esp.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "od_nvs";

static bool s_ready = false;

/* NVS has no offset read -- nvs_get_blob returns the whole blob or nothing -- so offsets are
 * served from one copy of the record. Filled on the first read after a change and dropped
 * whenever the medium changes under it. Affordable here (512 KB plus PSRAM) and the reason the
 * seam does not oblige BG22 to keep one. */
static uint8_t  s_cache[OD_HAL_NVS_MAX_RECORD];
static uint32_t s_cache_len = 0;
static bool     s_cache_valid = false;

int od_hal_nvs_init(void)
{
    if (s_ready) {
        return OD_HAL_NVS_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is unusable as-is: either full of dead entries or written by a newer
         * NVS format. Erasing loses stored config, which is acceptable here and nowhere else
         * -- the fleet transition is flash-and-reconfigure (docs/MIGRATION.md § "Deployed
         * fleet status"), so a unit arriving in this state is one that is about to be
         * reconfigured from the host anyway. Log it loudly: a device that silently forgets
         * its panel type looks like a hardware fault. */
        ESP_LOGW(TAG, "NVS unusable (%s) -- erasing partition; config will be lost",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }

    s_ready = true;
    return OD_HAL_NVS_OK;
}

/* Stored size straight from the medium. nvs_get_blob with a NULL destination resolves the
 * entry through the page index and reports its length without reading a data page, so the
 * core's header-first sequence costs one lookup rather than a record read. */
static int nvs_stored_size(nvs_handle_t h, size_t *stored)
{
    esp_err_t err = nvs_get_blob(h, OD_NVS_KEY, NULL, stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return OD_HAL_NVS_ENOENT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(size) failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_size(uint32_t *len_out)
{
    if (len_out == NULL) {
        return OD_HAL_NVS_EIO;
    }
    *len_out = 0;
    if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
        return OD_HAL_NVS_EIO;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OD_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return OD_HAL_NVS_ENOENT;   /* namespace absent = never provisioned */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(read) failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }

    size_t stored = 0;
    int rc = nvs_stored_size(h, &stored);
    nvs_close(h);
    if (rc != OD_HAL_NVS_OK) {
        return rc;
    }
    if (stored > sizeof(s_cache)) {
        /* Reachable only from a record written by other firmware or a larger build. Report
         * the medium's answer; refusing it is the core's decision, not this layer's. */
        ESP_LOGE(TAG, "stored record is %u B, this build caps at %u B",
                 (unsigned)stored, (unsigned)sizeof(s_cache));
    }
    *len_out = (uint32_t)stored;
    return OD_HAL_NVS_OK;
}

static int cache_fill(void)
{
    if (s_cache_valid) {
        return OD_HAL_NVS_OK;
    }
    if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
        return OD_HAL_NVS_EIO;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OD_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return OD_HAL_NVS_ENOENT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(read) failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }

    size_t stored = 0;
    int rc = nvs_stored_size(h, &stored);
    if (rc != OD_HAL_NVS_OK) {
        nvs_close(h);
        return rc;
    }
    if (stored > sizeof(s_cache)) {
        ESP_LOGE(TAG, "stored record is %u B, buffer is %u B",
                 (unsigned)stored, (unsigned)sizeof(s_cache));
        nvs_close(h);
        return OD_HAL_NVS_E2BIG;
    }

    err = nvs_get_blob(h, OD_NVS_KEY, s_cache, &stored);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }

    s_cache_len = (uint32_t)stored;
    s_cache_valid = true;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    if (buf == NULL) {
        return OD_HAL_NVS_EIO;
    }

    /* A zero-length read is still a question about the record, so presence and bounds are
     * answered before the span is dismissed as empty. */
    int rc = cache_fill();
    if (rc != OD_HAL_NVS_OK) {
        return rc;
    }
    /* Written as a subtraction so a 4 GB offset cannot wrap the sum past the record. */
    if (offset > s_cache_len || len > s_cache_len - offset) {
        return OD_HAL_NVS_E2BIG;
    }
    if (len == 0) {
        return OD_HAL_NVS_OK;
    }

    memcpy(buf, s_cache + offset, len);
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
    if (record == NULL || len == 0) {
        return OD_HAL_NVS_EIO;
    }
    if (len > sizeof(s_cache)) {
        return OD_HAL_NVS_E2BIG;
    }
    if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
        return OD_HAL_NVS_EIO;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(write) failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }

    err = nvs_set_blob(h, OD_NVS_KEY, record, (size_t)len);
    if (err == ESP_OK) {
        /* NVS writes are not durable until commit. Dropping it would make a save look
         * successful and vanish on reboot. */
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
        /* The medium may hold either record now: nvs_set_blob writes the new entry and only
         * then erases the old one, so ESP_ERR_NVS_REMOVE_FAILED arrives after the new bytes
         * are already visible. Drop the cache rather than guess -- the next read re-fills
         * from whatever is actually stored, which is the one answer that cannot be wrong. */
        s_cache_len = 0;
        s_cache_valid = false;
        return OD_HAL_NVS_EIO;
    }

    memcpy(s_cache, record, len);
    s_cache_len = len;
    s_cache_valid = true;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_erase(void)
{
    if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
        return OD_HAL_NVS_EIO;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_cache_len = 0;
        s_cache_valid = false;
        return OD_HAL_NVS_OK;   /* nothing stored -- erasing succeeded vacuously */
    }
    if (err != ESP_OK) {
        return OD_HAL_NVS_EIO;
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
        /* The record may or may not still be there. Invalidate rather than clear: the next
         * read re-fills from the medium, so a config that survived is still served and one
         * that did not is not. Clearing to empty would be the failure the contract names --
         * a device that forgets a config the medium still holds, and remembers it again on
         * the next boot. */
        s_cache_valid = false;
        return OD_HAL_NVS_EIO;
    }

    s_cache_len = 0;
    s_cache_valid = false;
    return OD_HAL_NVS_OK;
}

/* Drop the cached record. For od_hal_nvs_secure.c, which writes through the same NVS entry
 * from its own translation unit. */
void od_hal_nvs_esp_cache_drop(void)
{
    s_cache_len = 0;
    s_cache_valid = false;
}
