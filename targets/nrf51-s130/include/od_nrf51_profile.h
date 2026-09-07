#ifndef OD_NRF51_PROFILE_H
#define OD_NRF51_PROFILE_H

#define OD_NRF51_WIDTH                   104u
#define OD_NRF51_HEIGHT                  212u
#define OD_NRF51_IMAGE_BYTES             2756u
#define OD_NRF51_NOTIFY_MAX              20u
#define OD_NRF51_MAX_FRAME               244u
#define OD_NRF51_STATIC_CONFIG_MAX_SIZE  1024u
#define OD_NRF51_TRANSFER_TIMEOUT_MS     30000u
#define OD_NRF51_PANEL_TIMEOUT_MS        30000u
#define OD_NRF51_WATCHDOG_TIMEOUT_MS     30000u
#define OD_NRF51_HEARTBEAT_MS            10000u
#define OD_NRF51_AUTH_REPLY_ATTEMPTS     4u
#define OD_NRF51_VERSION_MAJOR           2u
#define OD_NRF51_VERSION_MINOR           0u
#define OD_NRF51_VERSION_PATCH           0u

#if OD_NRF51_WIDTH % 8u != 0u
#error "nRF51 monochrome width must be byte aligned"
#endif

#if (OD_NRF51_WIDTH / 8u) * OD_NRF51_HEIGHT != OD_NRF51_IMAGE_BYTES
#error "nRF51 image byte geometry mismatch"
#endif

#endif
