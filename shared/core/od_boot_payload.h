#ifndef OD_BOOT_PAYLOAD_H
#define OD_BOOT_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_BOOT_PAYLOAD_SIZE 23u
#define OD_BOOT_KEY_SIZE 16u
#define OD_BOOT_KEY_HEX_SIZE 33u
#define OD_BOOT_URL_SIZE 64u

bool od_boot_key_is_zero(const uint8_t key[OD_BOOT_KEY_SIZE]);

/* What the boot screen may say about the key, as one decision. Every renderer asks this rather
 * than re-deriving it: the panel layouts differ, the policy does not.
 *
 *   encryption_enabled == 0                      -> NOT_SET, whatever the show flag says
 *   encryption_enabled != 0, key all-zero/absent -> NOT_SET
 *   encryption_enabled != 0, key set, no flag    -> HIDDEN
 *   encryption_enabled != 0, key set, flag set   -> SHOWN
 *
 * NOT_SET is the only answer when no key is in force, so a device that demands nothing never
 * claims protection and never publishes a key -- on the panel or in the QR payload. */
enum od_boot_key_state {
    OD_BOOT_KEY_NOT_SET = 0,
    OD_BOOT_KEY_HIDDEN,
    OD_BOOT_KEY_SHOWN
};

struct SecurityConfig;

enum od_boot_key_state od_boot_key_state(const struct SecurityConfig *sec);

void od_boot_format_key_display(const uint8_t key[OD_BOOT_KEY_SIZE], bool show_key,
                                char out[OD_BOOT_KEY_HEX_SIZE]);

void od_boot_format_key_line(const char *label, const uint8_t *key_part, size_t key_part_len,
                             bool key_is_zero, bool show_key, char *out, size_t out_size);

void od_boot_payload_build(uint16_t legacy_tag_type, uint32_t device_id24,
                           const uint8_t key[OD_BOOT_KEY_SIZE], bool show_key,
                           uint16_t manufacturer_id,
                           uint8_t out[OD_BOOT_PAYLOAD_SIZE]);

size_t od_boot_base64url_encode(const uint8_t *data, size_t len, char *out, size_t out_size);

bool od_boot_url_build(uint16_t legacy_tag_type, uint32_t device_id24,
                       const uint8_t key[OD_BOOT_KEY_SIZE], bool show_key,
                       uint16_t manufacturer_id, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
