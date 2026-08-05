#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Matches the reference firmware (Firmware/src/config_parser.h) and the factory
 * generator's MAX_PACKET (scripts/factory_config_gen.py / tools/config_packet.py).
 * The stored record is [magic4][version4][crc4][data_len4][data[len]] = 16 + len
 * bytes, saved as a single Zephyr settings/NVS item. The nRF54L RRAM NVS sector
 * is 4096 B, so the largest storable record is ~sector - 4*ATE = 4064 B. The BLE
 * write paths cap an inbound config at MAX_CONFIG_CHUNKS(20)*CONFIG_CHUNK_SIZE(200)
 * = 4000 B (chunked) or 200 B (single-shot), so any client-writable config fits
 * (16 + 4000 = 4016 B < 4064 B). A blob larger than that is unreachable over BLE
 * and, if ever provisioned, fails cleanly via settings_save_one (see saveConfig).
 */
#define MAX_CONFIG_SIZE 4096

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t crc;
  uint32_t data_len;
  uint8_t data[MAX_CONFIG_SIZE];
} opendisplay_config_storage_t;

bool initConfigStorage(void);

bool saveConfig(uint8_t *config_data, uint32_t len);

bool loadConfig(uint8_t *config_data, uint32_t *len);

bool clearStoredConfig(void);

uint32_t calculateConfigCRC(uint8_t *data, uint32_t len);

#endif
