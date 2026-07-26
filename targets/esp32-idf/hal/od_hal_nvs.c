/* ESP-IDF NVS implementation of od_hal_nvs. See od_hal_nvs.h for the contract. */

#include "od_hal_nvs.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "od_nvs";

/* One namespace, one key. The blob is opaque here; its framing belongs to the core. */
#define OD_NVS_NAMESPACE "opendisplay"
#define OD_NVS_KEY       "config"

static bool s_ready = false;

int od_hal_nvs_init(void)
{
    if (s_ready) {
        return OD_HAL_NVS_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is unusable as-is: either full of dead entries or written by a newer
         * NVS format. Erasing loses stored config, which is acceptable here and nowhere else
         * -- the fleet transition is flash-and-reconfigure, so a unit arriving in this state
         * is one that is about to be reconfigured from the host anyway. Log it loudly: a
         * device that silently forgets its panel type looks like a hardware fault. */
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

int od_hal_nvs_load(uint8_t *buf, uint32_t cap, uint32_t *len_out)
{
    if (buf == NULL || len_out == NULL || cap == 0) {
        return OD_HAL_NVS_EIO;
    }
    if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
        return OD_HAL_NVS_EIO;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OD_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *len_out = 0;
        return OD_HAL_NVS_ENOENT;   /* namespace absent = never provisioned */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(read) failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }

    /* Ask for the size first: nvs_get_blob with a NULL out-pointer reports the stored length,
     * so an oversized record is rejected before it can overrun the caller's buffer rather
     * than after. */
    size_t stored = 0;
    err = nvs_get_blob(h, OD_NVS_KEY, NULL, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        *len_out = 0;
        return OD_HAL_NVS_ENOENT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(size) failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return OD_HAL_NVS_EIO;
    }
    if (stored > (size_t)cap) {
        ESP_LOGE(TAG, "stored config is %u B, buffer is %u B", (unsigned)stored, (unsigned)cap);
        nvs_close(h);
        return OD_HAL_NVS_E2BIG;
    }

    err = nvs_get_blob(h, OD_NVS_KEY, buf, &stored);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }

    *len_out = (uint32_t)stored;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_save(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0) {
        return OD_HAL_NVS_EIO;
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

    err = nvs_set_blob(h, OD_NVS_KEY, buf, (size_t)len);
    if (err == ESP_OK) {
        /* NVS writes are not durable until commit. The LittleFS path this replaces closed the
         * file, which flushed; dropping the commit would make a save look successful and
         * vanish on reboot. */
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
        return OD_HAL_NVS_EIO;
    }
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

    return (err == ESP_OK) ? OD_HAL_NVS_OK : OD_HAL_NVS_EIO;
}
