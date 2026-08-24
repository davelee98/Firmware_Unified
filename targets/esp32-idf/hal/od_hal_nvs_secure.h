/* Overwrite the stored config record with zeros, then remove it. ESP32 only.
 *
 * NOT part of shared/hal/od_hal_nvs.h: one target defines it and one call site uses it, and a
 * shared header carrying a function the other targets do not define is either a link error
 * there or dead weight. If a second target ever needs this, widen the shared seam then.
 *
 * It exists because the record carries the AES-128 master key from config packet 0x27, and a
 * plain erase is not enough: nvs_erase_key() marks the entry deleted but leaves its bytes in
 * the flash sector until a garbage-collection pass reclaims it, so the key stays recoverable
 * from a raw dump.
 *
 * BEST-EFFORT, deliberately. NVS is log-structured, so the zero-write lands in a NEW entry
 * rather than on top of the old bytes. What it does guarantee is that the erase is preceded by
 * a same-size write, which forces NVS to consider the page full and makes reclamation of the
 * old entry likely rather than incidental. A guaranteed wipe needs nvs_flash_erase() on the
 * whole partition, which also destroys pairing and panel config -- the caller's decision, not
 * this function's. See docs/FOLLOWUPS.md.
 *
 * Returns the od_hal_nvs status codes.
 */

#ifndef OD_HAL_NVS_SECURE_H
#define OD_HAL_NVS_SECURE_H

#ifdef __cplusplus
extern "C" {
#endif

int od_hal_nvs_secure_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_NVS_SECURE_H */
