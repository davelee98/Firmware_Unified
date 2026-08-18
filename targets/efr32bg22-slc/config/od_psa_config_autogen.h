/*
 * Tracked PSA capability selection for the hand-maintained BG22 CMake build.
 *
 * The SLC-generated sli_psa_config_autogen.h lives under ignored autogen/, so editing it does not
 * make a clean checkout reproducible. psa_crypto_config.h supports an override specifically for
 * this purpose; cmake_gcc selects this file while opendisplay-bg22.slcp carries the equivalent
 * component choices for a future safe regeneration.
 */
#ifndef OD_PSA_CONFIG_AUTOGEN_H
#define OD_PSA_CONFIG_AUTOGEN_H

#define PSA_WANT_KEY_TYPE_AES 1
#define PSA_WANT_ALG_ECB_NO_PADDING 1
#define PSA_WANT_ALG_CMAC 1
#define PSA_WANT_ALG_CCM 1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1
#define PSA_WANT_ECC_SECP_R1_256 1
#define PSA_WANT_ALG_ECDH 1
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

#if defined(KSU_MAX_KEY_SLOTS)
#include "sli_ksu_keyslots_config.h"
#define MBEDTLS_PSA_KEY_SLOT_COUNT \
  (2 + 1 + SL_PSA_KEY_USER_SLOT_COUNT + SLI_KSU_MAX_KEY_SLOTS - SLI_KSU_KEY_SLOT_USER_START + 1)
#else
#define MBEDTLS_PSA_KEY_SLOT_COUNT (2 + 1 + SL_PSA_KEY_USER_SLOT_COUNT + 1)
#endif

#ifndef SL_PSA_ITS_MAX_FILES
#define SL_PSA_ITS_MAX_FILES (1 + SL_PSA_ITS_USER_MAX_FILES)
#endif

#endif /* OD_PSA_CONFIG_AUTOGEN_H */
