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

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_NVS_H */
