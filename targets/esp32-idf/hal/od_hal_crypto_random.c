/* od_hal_crypto_random -- this target's RNG arm of shared/hal/od_hal_crypto.h.
 *
 * Its own translation unit so tests/host can compile the production function against a fake
 * backend; the rest of the HAL is mbedTLS and does not build on a workstation.
 */

#include "od_hal_crypto.h"

#include "esp_random.h"

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    if (buf == NULL && len != 0u) {
        return OD_HAL_CRYPTO_ERROR;
    }
    /* esp_fill_random() returns void, so this cannot fail and cannot honour the shared contract's
     * "never offer a challenge the device cannot honour" path. C11.1 replaces it. */
    esp_fill_random(buf, len);
    return OD_HAL_CRYPTO_OK;
}
