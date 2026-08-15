/* od_hal_crypto for Zephyr on nRF over PSA Crypto. Contract: shared/hal/od_hal_crypto.h. */

#include "od_hal_crypto.h"
#include "od_log.h"

#include <psa/crypto.h>

#define OD_PSA_CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OD_HAL_CRYPTO_TAG_LEN)

static psa_key_id_t s_slots[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_slot_ready[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_crypto_ready;

static enum od_hal_crypto_status crypto_init_once(void)
{
    psa_status_t status;

    if (s_crypto_ready) {
        return OD_HAL_CRYPTO_OK;
    }
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA init failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_crypto_ready = true;
    return OD_HAL_CRYPTO_OK;
}

static bool slot_valid(od_hal_crypto_slot_t slot)
{
    return slot < OD_HAL_CRYPTO_KEY_SLOTS;
}

static enum od_hal_crypto_status slot_release(od_hal_crypto_slot_t slot)
{
    psa_status_t status;

    if (!s_slot_ready[slot]) {
        return OD_HAL_CRYPTO_OK;
    }
    status = psa_destroy_key(s_slots[slot]);
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA key destroy failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_slots[slot] = 0;
    s_slot_ready[slot] = false;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot,
                                                const uint8_t key[16])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) || key == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (slot_release(slot) != OD_HAL_CRYPTO_OK) {
        return OD_HAL_CRYPTO_ERROR;
    }

    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    /* The key policy includes the 12-byte tag. Plain PSA_ALG_CCM pins a 16-byte tag and rejects
     * every wire-compatible operation with PSA_ERROR_NOT_PERMITTED. */
    psa_set_key_algorithm(&attr, OD_PSA_CCM_ALG);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    status = psa_import_key(&attr, key, 16, &s_slots[slot]);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA CCM key import failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_slot_ready[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    if (crypto_init_once() == OD_HAL_CRYPTO_OK && slot_valid(slot)) {
        (void)slot_release(slot);
    }
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len)
{
    size_t output_len = 0;
    uint32_t required = (uint32_t)plain_len + OD_HAL_CRYPTO_TAG_LEN;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || (plain == NULL && plain_len != 0u) ||
        ct == NULL || ct_len == NULL || required > ct_cap) {
        return OD_HAL_CRYPTO_ERROR;
    }

    status = psa_aead_encrypt(s_slots[slot], OD_PSA_CCM_ALG, nonce, nonce_len,
                              aad, aad_len, plain, plain_len, ct, ct_cap, &output_len);
    if (status != PSA_SUCCESS || output_len != required) {
        od_log_error("crypto: PSA CCM encrypt failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    *ct_len = (uint16_t)output_len;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len)
{
    size_t output_len = 0;
    uint16_t expected_len;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || ct == NULL ||
        ct_len <= OD_HAL_CRYPTO_TAG_LEN || plain == NULL || plain_len == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    expected_len = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    if (plain_cap < expected_len) {
        return OD_HAL_CRYPTO_ERROR;
    }

    status = psa_aead_decrypt(s_slots[slot], OD_PSA_CCM_ALG, nonce, nonce_len,
                              aad, aad_len, ct, ct_len, plain, plain_cap, &output_len);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        return OD_HAL_CRYPTO_AUTH_FAILED;
    }
    if (status != PSA_SUCCESS || output_len != expected_len) {
        od_log_error("crypto: PSA CCM decrypt failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    *plain_len = (uint16_t)output_len;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t mac_len = 0;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL ||
        (msg == NULL && msg_len != 0u) || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    status = psa_import_key(&attr, key, 16, &key_id);
    psa_reset_key_attributes(&attr);
    if (status == PSA_SUCCESS) {
        status = psa_mac_compute(key_id, PSA_ALG_CMAC, msg, msg_len, out, 16, &mac_len);
        (void)psa_destroy_key(key_id);
    }
    if (status != PSA_SUCCESS || mac_len != 16u) {
        od_log_error("crypto: PSA CMAC failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t out_len = 0;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL || in == NULL || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    status = psa_import_key(&attr, key, 16, &key_id);
    psa_reset_key_attributes(&attr);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &out_len);
        (void)psa_destroy_key(key_id);
    }
    if (status != PSA_SUCCESS || out_len != 16u) {
        od_log_error("crypto: PSA AES-ECB failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || (buf == NULL && len != 0u)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    status = psa_generate_random(buf, len);
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA random failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}
