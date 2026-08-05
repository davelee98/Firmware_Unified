#include "factory_config.h"

#include <stdio.h>
#include <string.h>

#include "opendisplay_config_storage.h"

/*
 * CRC-16/CCITT-FALSE over the outer config packet body with the two length
 * bytes forced to zero — the canonical toolbox config CRC, matching the
 * reference firmware (factory_config.cpp toolboxOuterCrc) and the generator
 * (tools/config_packet.py outer_packet_crc). Kept local so this file has no
 * dependency on the parser's static helper.
 */
static uint16_t factory_outer_crc16(const uint8_t *data, uint32_t body_len)
{
  uint16_t crc = 0xFFFFu;

  for (uint32_t i = 0; i < body_len; i++) {
    const uint8_t b = (i < 2u) ? 0u : data[i];

    crc ^= (uint16_t)((uint16_t)b << 8);
    for (int bit = 0; bit < 8; bit++) {
      if ((crc & 0x8000u) != 0u) {
        crc = (uint16_t)(((uint32_t)crc << 1) ^ 0x1021u);
      } else {
        crc = (uint16_t)((uint32_t)crc << 1);
      }
    }
  }
  return crc;
}

static bool factory_packet_valid(const uint8_t *data, uint32_t len)
{
  if (len < 4u || len > MAX_CONFIG_SIZE) {
    return false;
  }
  const uint16_t declared = (uint16_t)data[0] | ((uint16_t)data[1] << 8);

  if (declared != len) {
    return false;
  }
  const uint16_t calc = factory_outer_crc16(data, len - 2u);
  const uint16_t given = (uint16_t)data[len - 2u] | ((uint16_t)data[len - 1u] << 8);

  return given == calc;
}

#ifdef FACTORY_HAS_EMBED
static bool factory_embed_present(const factory_flash_cfg_t *fc)
{
  if (fc == NULL || fc->magic != FACTORY_CFG_MAGIC) {
    return false;
  }
  if (fc->len < 4u || fc->len > MAX_CONFIG_SIZE) {
    return false;
  }
  return factory_packet_valid(fc->data, fc->len);
}
#endif

bool tryProvisionFactoryEmbed(void)
{
#ifdef FACTORY_HAS_EMBED
  if (!factory_embed_present(&g_factory_embed)) {
    return false;
  }

  printf("No valid stored config; provisioning from factory embed...\r\n");
  if (saveConfig((uint8_t *)(void *)g_factory_embed.data, g_factory_embed.len)) {
    printf("Factory config saved to settings\r\n");
    return true;
  }

  printf("ERROR: Factory embed present but saveConfig failed\r\n");
#endif
  return false;
}
