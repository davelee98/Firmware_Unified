#ifndef FACTORY_CONFIG_H
#define FACTORY_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_config_storage.h" /* MAX_CONFIG_SIZE */

/*
 * Optional build-time factory config embed. scripts/factory_config_gen.py runs as
 * a PlatformIO pre-build step and (when OPENDISPLAY_FACTORY_CONFIG_HEX / the
 * custom_factory_config_hex option is set) writes src/generated/factory_config_data.c
 * defining g_factory_embed and adds -DFACTORY_HAS_EMBED. Otherwise it writes a
 * stub and this file's provisioning path compiles to a no-op. Ported from the
 * reference firmware (Firmware/src/factory_config.{h,cpp}); the generator's
 * MAX_PACKET (tools/config_packet.py) equals MAX_CONFIG_SIZE so data[] fits the
 * padded blob exactly.
 */
#define FACTORY_CFG_MAGIC 0xFAC70A5Au

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t len;
  uint8_t data[MAX_CONFIG_SIZE];
} factory_flash_cfg_t;

#ifdef FACTORY_HAS_EMBED
extern const factory_flash_cfg_t g_factory_embed;
#endif

/*
 * If a valid factory embed is present and no valid config is stored yet, persist
 * the embedded packet to settings/NVS. Returns true only if a config was written.
 */
bool tryProvisionFactoryEmbed(void);

#endif
