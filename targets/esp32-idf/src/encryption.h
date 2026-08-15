#ifndef ENCRYPTION_H
#define ENCRYPTION_H

#include <stddef.h>
#include <stdint.h>

void getAuthDeviceIdBytes(uint8_t* device_id);
bool isEncryptionEnabled();
bool isAuthenticated();
void clearEncryptionSession();
bool checkEncryptionSessionTimeout();
void updateEncryptionSessionActivity();
bool handleAuthenticate(uint8_t* data, uint16_t len);
/// Derive the 16-byte TLS-PSK for the LAN TLS channel from the configured master
/// key via AES-CMAC over a fixed KDF label. Returns false if encryption is not
/// configured (no usable master key). The matching PSK IDENTITY is "opendisplay".
bool deriveTlsPsk(uint8_t* psk_out16);

/* The device identity string: exactly OD_CHIP_ID_HEX_LEN uppercase hex digits, zero-padded,
 * NUL-terminated. `out` needs OD_CHIP_ID_HEX_LEN + 1 bytes; a smaller buffer yields an empty
 * string rather than a truncated id, because a truncated device id is worse than none -- it is
 * a DIFFERENT device as far as the host is concerned.
 *
 * Was `String getChipIdHex()`. Arduino's String is forbidden in shared/ outright (CLAUDE.md),
 * and this was the only one left on an interface rather than inside a function body, which is
 * why it is its own phase C step. Every caller already padded to six digits by hand; that is
 * now the contract instead of a convention. */
#define OD_CHIP_ID_HEX_LEN 6
void getChipIdHex(char* out, size_t out_size);
void secureEraseConfig();
void checkResetPin();

#endif
