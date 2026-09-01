/* od_hal_crypto for ESP-IDF over mbedTLS. Contract: shared/hal/od_hal_crypto.h.
 *
 * od_hal_crypto_random() is NOT here: it lives in od_hal_crypto_random.c so a host test can
 * compile it against a fake RNG backend without dragging mbedTLS in. Same contract, same
 * translation-unit-local state, one file each. */

#include "od_hal_crypto.h"
#include "od_log.h"

#include "mbedtls/aes.h"
#include "mbedtls/ccm.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"

static mbedtls_ccm_context s_slots[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_slot_ready[OD_HAL_CRYPTO_KEY_SLOTS];

/* mbedTLS needs per-context initialisation but has no process-wide crypto init. Keeping the
 * idempotent backend hook explicit makes every public entry point satisfy the shared contract. */
static enum od_hal_crypto_status crypto_init_once(void)
{
    return OD_HAL_CRYPTO_OK;
}

static bool slot_valid(od_hal_crypto_slot_t slot)
{
    return slot < OD_HAL_CRYPTO_KEY_SLOTS;
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot,
                                                const uint8_t key[16])
{
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) || key == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (s_slot_ready[slot]) {
        mbedtls_ccm_free(&s_slots[slot]);
        s_slot_ready[slot] = false;
    }

    mbedtls_ccm_init(&s_slots[slot]);
    ret = mbedtls_ccm_setkey(&s_slots[slot], MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        od_log_error("mbedTLS CCM setkey failed: %d", ret);
        mbedtls_ccm_free(&s_slots[slot]);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_slot_ready[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    (void)crypto_init_once();
    if (slot_valid(slot) && s_slot_ready[slot]) {
        mbedtls_ccm_free(&s_slots[slot]);
        s_slot_ready[slot] = false;
    }
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len)
{
    uint32_t required = (uint32_t)plain_len + OD_HAL_CRYPTO_TAG_LEN;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || (plain == NULL && plain_len != 0u) ||
        ct == NULL || ct_len == NULL || required > ct_cap) {
        return OD_HAL_CRYPTO_ERROR;
    }

    ret = mbedtls_ccm_encrypt_and_tag(&s_slots[slot], plain_len, nonce, nonce_len,
                                      aad, aad_len, plain, ct, &ct[plain_len],
                                      OD_HAL_CRYPTO_TAG_LEN);
    if (ret != 0) {
        od_log_error("mbedTLS CCM encrypt failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    *ct_len = (uint16_t)required;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len)
{
    uint16_t cipher_len;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || ct == NULL ||
        ct_len <= OD_HAL_CRYPTO_TAG_LEN || plain == NULL || plain_len == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    cipher_len = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    if (plain_cap < cipher_len) {
        return OD_HAL_CRYPTO_ERROR;
    }

    ret = mbedtls_ccm_auth_decrypt(&s_slots[slot], cipher_len, nonce, nonce_len,
                                   aad, aad_len, ct, plain, &ct[cipher_len],
                                   OD_HAL_CRYPTO_TAG_LEN);
    if (ret == MBEDTLS_ERR_CCM_AUTH_FAILED) {
        return OD_HAL_CRYPTO_AUTH_FAILED;
    }
    if (ret != 0) {
        od_log_error("mbedTLS CCM decrypt failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    *plain_len = cipher_len;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    const mbedtls_cipher_info_t *cipher_info;
    mbedtls_cipher_context_t ctx;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL ||
        (msg == NULL && msg_len != 0u) || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    cipher_info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (cipher_info == NULL) {
        od_log_error("AES-128-ECB cipher info unavailable");
        return OD_HAL_CRYPTO_ERROR;
    }

    mbedtls_cipher_init(&ctx);
    ret = mbedtls_cipher_setup(&ctx, cipher_info);
    if (ret == 0) {
        ret = mbedtls_cipher_cmac_starts(&ctx, key, 128);
    }
    if (ret == 0) {
        ret = mbedtls_cipher_cmac_update(&ctx, msg, msg_len);
    }
    if (ret == 0) {
        ret = mbedtls_cipher_cmac_finish(&ctx, out);
    }
    mbedtls_cipher_free(&ctx);
    if (ret != 0) {
        od_log_error("mbedTLS CMAC failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    mbedtls_aes_context aes;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL || in == NULL || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    mbedtls_aes_init(&aes);
    ret = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
    }
    mbedtls_aes_free(&aes);
    if (ret != 0) {
        od_log_error("mbedTLS AES-ECB failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}
