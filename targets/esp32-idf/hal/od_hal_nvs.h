/* od_hal_nvs -- config blob persistence for the ESP32 target.
 *
 * This is the first piece of this target written to a shared/hal interface rather than to
 * whatever the Arduino code happened to use. The signatures are exactly those specified in
 * docs/SHARED_API_DESIGN.md § od_hal_nvs, so promoting the config subsystem into shared/core
 * later is a repoint rather than a rewrite.
 *
 * The HAL stores an OPAQUE BLOB. Record framing -- magic, version, inner CRC32, length -- is
 * the core's business and stays in config_parser.cpp, per SHARED_API_DESIGN: "The core owns
 * the record framing ... so the HAL stores an opaque blob."
 *
 * Backend: ESP-IDF NVS, replacing LittleFS. Decided 2026-07-25. Deployed ESP32 units hold
 * their config in LittleFS and nothing migrates it: the fleet transition is flash-and-
 * reconfigure (docs/MIGRATION.md § "Deployed fleet status"), so a unit is reflashed and then
 * reconfigured from the host. That decision is what makes going straight to NVS cheaper than
 * shimming LittleFS and replacing it later.
 */

#ifndef OD_HAL_NVS_H
#define OD_HAL_NVS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convention (SHARED_API_DESIGN.md § shared/hal): 0 on success, negative on failure. */
#define OD_HAL_NVS_OK        0
#define OD_HAL_NVS_ENOENT   -2   /* no stored record -- an empty device, not an error */
#define OD_HAL_NVS_EIO      -5
#define OD_HAL_NVS_E2BIG    -7   /* stored record is larger than the caller's buffer */

/* Largest record this HAL will handle: the config header plus MAX_CONFIG_SIZE (4096 on every
 * target since 2026-07-25). Only od_hal_nvs_secure_erase() needs it, to size its zero buffer
 * without including the config layer's headers. */
#define OD_HAL_NVS_MAX_RECORD  4160u

/* One-time init. Safe to call more than once. */
int od_hal_nvs_init(void);

/* Load the stored blob into buf. `cap` is the caller's capacity -- the core passes
 * MAX_CONFIG_SIZE, which since 2026-07-25 is a uniform 4096 on every target.
 * Returns OD_HAL_NVS_ENOENT when nothing is stored, which callers must treat as
 * "unprovisioned device", never as a failure. */
int od_hal_nvs_load(uint8_t *buf, uint32_t cap, uint32_t *len_out);

/* Store len bytes, replacing any previous record. */
int od_hal_nvs_save(const uint8_t *buf, uint32_t len);

/* Remove the stored record. Succeeds when there was nothing to remove. */
int od_hal_nvs_erase(void);

/* Overwrite the stored record with zeros, then remove it.
 *
 * For the security wipe (encryption.cpp's secureEraseConfig), which exists because the record
 * contains the AES-128 master key from config packet 0x27. A plain erase is NOT sufficient:
 * nvs_erase_key() marks the entry deleted but leaves its bytes in the flash sector until a
 * garbage-collection pass happens to reclaim it, so the key stays recoverable from a raw dump.
 *
 * This is best-effort, and deliberately documented as such: NVS is log-structured, so the
 * zero-write lands in a NEW entry rather than on top of the old bytes. What it does guarantee
 * is that the erase is preceded by a same-size write, which is what forces NVS to consider the
 * page full and makes reclamation of the old entry likely rather than incidental. A guaranteed
 * wipe needs nvs_flash_erase() on the whole partition -- which also destroys the device's
 * pairing and panel config, so it is the caller's decision, not this function's.
 * See docs/FOLLOWUPS.md. */
int od_hal_nvs_secure_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_NVS_H */
