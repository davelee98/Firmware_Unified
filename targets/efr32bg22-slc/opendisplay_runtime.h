#ifndef OPENDISPLAY_RUNTIME_H
#define OPENDISPLAY_RUNTIME_H

/*
 * Firmware-local (RAM-only) OpenDisplay types and flags.
 *
 * These are intentionally NOT part of the shared wire-protocol header
 * opendisplay_structs.h. That header is meant to be a byte-for-byte vendored
 * copy of opendisplay-protocol/src/opendisplay_structs.h (managed by
 * tools/sync_protocol_header.py), and the canonical header explicitly excludes
 * repo-specific / in-memory types such as GlobalConfig and EncryptionSession.
 *
 * Keeping them here lets opendisplay_structs.h become a clean vendored copy
 * while this firmware-owned header holds everything Silabs-specific.
 *
 * The wire structs referenced below (SystemConfig, SecurityConfig, ...) come
 * from opendisplay_structs.h, included here.
 */

#include <stdbool.h>
#include <stdint.h>

#include "od_config.h"

/* Preserve the target's source-level spelling while using the shared aggregate. */
#define GlobalConfig od_config

/* Firmware-local interpretation of the SecurityConfig.flags bitfield
 * (SecurityConfig itself is a wire struct in opendisplay_structs.h). */
#define SECURITY_FLAG_REWRITE_ALLOWED     (1 << 0)
#define SECURITY_FLAG_SHOW_KEY_ON_SCREEN  (1 << 1)
#define SECURITY_FLAG_RESET_PIN_ENABLED   (1 << 2)
#define SECURITY_FLAG_RESET_PIN_POLARITY  (1 << 3)
#define SECURITY_FLAG_RESET_PIN_PULLUP    (1 << 4)
#define SECURITY_FLAG_RESET_PIN_PULLDOWN  (1 << 5)

#endif /* OPENDISPLAY_RUNTIME_H */
