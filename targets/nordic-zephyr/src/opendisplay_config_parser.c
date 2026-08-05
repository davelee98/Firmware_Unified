#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_device_flags.h"
#include <stdio.h>
#include <string.h>

static struct SecurityConfig s_od_security_parsed;

static bool security_key_bytes_set(const uint8_t key[16])
{
  if (key[0] != 0u) {
    return true;
  }
  return memcmp(key, key + 1, 15) != 0;
}

const struct SecurityConfig *od_get_parsed_security(void)
{
  return &s_od_security_parsed;
}

bool od_security_key_set(void)
{
  return security_key_bytes_set(s_od_security_parsed.encryption_key);
}

static void data_extended_null_terminate(struct DataExtended *de)
{
  de->manufacturer_name[31] = '\0';
  de->model_name[31] = '\0';
  de->serial_number[31] = '\0';
  de->friendly_name[31] = '\0';
  de->device_location[31] = '\0';
  de->device_id[31] = '\0';
  de->custom_string_1[31] = '\0';
  de->custom_string_2[31] = '\0';
  de->custom_string_3[31] = '\0';
}

/*
 * If an unknown packet (e.g. 0x2C before this was implemented) caused the main
 * parser to jump to CRC, security_config (0x27) after it was never loaded.
 * Scan the raw blob for a security packet as a fallback.
 */
static void rescan_security_packet(const uint8_t *configData, uint32_t configLen)
{
  if (od_security_key_set() || s_od_security_parsed.encryption_enabled != 0u) {
    return;
  }
  for (uint32_t i = 3u; i + 2u + sizeof(struct SecurityConfig) <= configLen - 2u; i++) {
    if (configData[i + 1u] != CONFIG_PKT_SECURITY) {
      continue;
    }
    const struct SecurityConfig *candidate =
        (const struct SecurityConfig *)&configData[i + 2u];
    if (candidate->encryption_enabled != 0u ||
        security_key_bytes_set(candidate->encryption_key)) {
      memcpy(&s_od_security_parsed, candidate, sizeof(struct SecurityConfig));
      printf("Security: recovered from scan @%u (enabled=%d)\r\n",
             (unsigned)i, (int)s_od_security_parsed.encryption_enabled);
      return;
    }
  }
}


#define TRANSMISSION_MODE_CLEAR_ON_BOOT (1 << 7)

static uint16_t crc16_ccitt_feed(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)((uint16_t)b << 8);
    for (int j = 0; j < 8; j++) {
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)(((uint32_t)crc << 1) ^ 0x1021U);
        } else {
            crc = (uint16_t)((uint32_t)crc << 1);
        }
    }
    return crc;
}

static uint16_t config_toolbox_outer_crc16(const uint8_t *data, uint32_t body_len)
{
    if (body_len < 2U) {
        uint16_t crc = 0xFFFFU;

        for (uint32_t i = 0; i < body_len; i++) {
            crc = crc16_ccitt_feed(crc, data[i]);
        }
        return crc;
    }
    uint16_t crc = 0xFFFFU;

    crc = crc16_ccitt_feed(crc, 0);
    crc = crc16_ccitt_feed(crc, 0);
    for (uint32_t i = 2U; i < body_len; i++) {
        crc = crc16_ccitt_feed(crc, data[i]);
    }
    return crc;
}

/*
 * On-wire data size (excluding the 2-byte [number][type] header) of every known
 * config packet type. Returns 0 for a genuinely unknown type.
 *
 * These sizes are cross-checked three ways and all agree: the reference firmware
 * structs (Firmware/src/structs.h), this port's structs (opendisplay_structs.h),
 * and the py-opendisplay serializer docstrings (protocol/config_serializer.py).
 * The one place the old code disagreed with the wire was wifi_config (0x26):
 * this port skipped a hardcoded 162 bytes, but the serializer emits 160
 * ("Serialize WifiConfig to 160 bytes") and the reference struct is 160 — the
 * extra 2 bytes desynced every packet after wifi. Corrected to 160 here.
 *
 * 0x28 (touch_controller) and 0x29 (passive_buzzer) have no parse case on this
 * branch, but are sized here so the default branch skips them by their true size
 * instead of dropping every packet after them. Sibling PRs add real parse cases;
 * the sized skip is exactly what lets those branches stay independent.
 */
static uint16_t config_packet_data_size(uint8_t packetId)
{
    switch (packetId) {
        case CONFIG_PKT_SYSTEM:        return 22u;   /* 0x01 system_config      */
        case CONFIG_PKT_MANUFACTURER:  return 22u;   /* 0x02 manufacturer_data  */
        case CONFIG_PKT_POWER:         return 30u;   /* 0x04 power_option       */
        case CONFIG_PKT_DISPLAY:       return 46u;   /* 0x20 display            */
        case CONFIG_PKT_LED:           return 22u;   /* 0x21 led                */
        case CONFIG_PKT_SENSOR:        return 30u;   /* 0x23 sensor_data        */
        case CONFIG_PKT_DATA_BUS:      return 30u;   /* 0x24 data_bus           */
        case CONFIG_PKT_BINARY_INPUT:  return 30u;   /* 0x25 binary_inputs      */
        case CONFIG_PKT_WIFI:          return 160u;  /* 0x26 wifi_config        */
        case CONFIG_PKT_SECURITY:      return 64u;   /* 0x27 security_config    */
        case CONFIG_PKT_TOUCH:         return 32u;   /* 0x28 touch_controller   */
        case CONFIG_PKT_PASSIVE_BUZZER:return 32u;   /* 0x29 passive_buzzer     */
        case CONFIG_PKT_NFC:           return 32u;   /* 0x2A nfc_config         */
        case CONFIG_PKT_FLASH:         return 32u;   /* 0x2B flash_config       */
        case CONFIG_PKT_DATA_EXTENDED: return 288u;  /* 0x2C data_extended      */
        default:                       return 0u;
    }
}

bool parseConfigBytes(uint8_t* configData, uint32_t configLen, struct GlobalConfig* globalConfig) {
    if (globalConfig == NULL || configData == NULL) {
        printf("Invalid parameters for parseConfigBytes\n");
        return false;
    }
    
    memset(globalConfig, 0, sizeof(struct GlobalConfig));
    memset(&s_od_security_parsed, 0, sizeof(s_od_security_parsed));
    globalConfig->data_extended_loaded = false;
    
    if (configLen < 3) {
        printf("Config too short: %u bytes\r\n", (unsigned)configLen);
        globalConfig->loaded = false;
        return false;
    }
    
    printf("Parsing config: %u bytes\r\n", (unsigned)configLen);
    
    uint32_t offset = 0;
    offset += 2;
    
    globalConfig->version = configData[offset++];
    globalConfig->minor_version = 0; // Not stored in current format
    
    uint32_t packetIndex = 0;
    while (offset < configLen - 2) { // -2 for CRC
        if (offset > configLen) {
            printf("Offset overflow: offset=%u > configLen=%u\r\n", (unsigned)offset, (unsigned)configLen);
            globalConfig->loaded = false;
            return false;
        }
        
        uint32_t remaining = configLen - 2 - offset;
        if (offset + 2 > configLen - 2) {
            printf("Loop exit: not enough for header (need 2, have %u)\r\n", (unsigned)remaining);
            break;
        }
        
        uint8_t packetNum = configData[offset];
        uint8_t packetId = configData[offset + 1];
        offset += 2; // Advance past packet header
        
        if (offset > configLen) {
            printf("Offset overflow after header: offset=%u > configLen=%u\r\n", (unsigned)offset, (unsigned)configLen);
            globalConfig->loaded = false;
            return false;
        }
        
        packetIndex++; // Count this packet (before processing, so we count even if we skip it)
        if (packetId == CONFIG_PKT_SYSTEM || packetId == CONFIG_PKT_MANUFACTURER || 
            packetId == CONFIG_PKT_POWER || packetId == CONFIG_PKT_DISPLAY) {
            printf("Pkt #%u ID=0x%02X\r\n", (unsigned)packetNum, packetId);
        }
        
        switch (packetId) {
            case CONFIG_PKT_SYSTEM: // system_config
                if (offset > configLen) {
                    printf("Offset overflow before system_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct SystemConfig) <= configLen - 2) {
                    memcpy(&globalConfig->system_config, &configData[offset], sizeof(struct SystemConfig));
                    offset += sizeof(struct SystemConfig);
                    if ((globalConfig->system_config.device_flags & DEVICE_FLAG_CHANNEL_SOUNDING) != 0u) {
                        printf("system_config: CHANNEL_SOUNDING enabled\r\n");
                    }
                    if (offset > configLen) {
                        printf("Offset overflow after system_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("system_config: need %zu, have %u\r\n", sizeof(struct SystemConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_MANUFACTURER: // manufacturer_data
                if (offset > configLen) {
                    printf("Offset overflow before manufacturer_data\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct ManufacturerData) <= configLen - 2) {
                    memcpy(&globalConfig->manufacturer_data, &configData[offset], sizeof(struct ManufacturerData));
                    offset += sizeof(struct ManufacturerData);
                    if (offset > configLen) {
                        printf("Offset overflow after manufacturer_data\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("manufacturer_data: need %zu, have %u\r\n", sizeof(struct ManufacturerData), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_POWER: // power_option
                if (offset > configLen) {
                    printf("Offset overflow before power_option\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct PowerOption) <= configLen - 2) {
                    memcpy(&globalConfig->power_option, &configData[offset], sizeof(struct PowerOption));
                    offset += sizeof(struct PowerOption);
                    if (offset > configLen) {
                        printf("Offset overflow after power_option\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("power_option: need %zu, have %u\r\n", sizeof(struct PowerOption), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_DISPLAY: // display
                if (offset > configLen) {
                    printf("Offset overflow before display\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->display_count < 4 && offset + sizeof(struct DisplayConfig) <= configLen - 2) {
                    memcpy(&globalConfig->displays[globalConfig->display_count], &configData[offset], sizeof(struct DisplayConfig));
                    printf("Display: ic=0x%04X %dx%d\r\n", 
                                 globalConfig->displays[globalConfig->display_count].panel_ic_type,
                                 globalConfig->displays[globalConfig->display_count].pixel_width,
                                 globalConfig->displays[globalConfig->display_count].pixel_height);
                    printf("Display: RST=%d BUSY=%d DC=%d\r\n", 
                                 globalConfig->displays[globalConfig->display_count].reset_pin,
                                 globalConfig->displays[globalConfig->display_count].busy_pin,
                                 globalConfig->displays[globalConfig->display_count].dc_pin);
                    printf("Display: CS=%d DATA=%d CLK=%d\r\n", 
                                 globalConfig->displays[globalConfig->display_count].cs_pin,
                                 globalConfig->displays[globalConfig->display_count].data_pin,
                                 globalConfig->displays[globalConfig->display_count].clk_pin);
                    printf("Display: color=%d modes=0x%02X\r\n", 
                                 globalConfig->displays[globalConfig->display_count].color_scheme,
                                 globalConfig->displays[globalConfig->display_count].transmission_modes);
                    offset += sizeof(struct DisplayConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after display\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->display_count++;
                } else if (globalConfig->display_count >= 4) {
                    offset += sizeof(struct DisplayConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after display (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("display: need %zu, have %u\r\n", sizeof(struct DisplayConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_LED: // led - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before led\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->led_count < 4 && offset + sizeof(struct LedConfig) <= configLen - 2) {
                    memcpy(&globalConfig->leds[globalConfig->led_count], &configData[offset], sizeof(struct LedConfig));
                    offset += sizeof(struct LedConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after led\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->led_count++;
                } else if (globalConfig->led_count >= 4) {
                    offset += sizeof(struct LedConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after led (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("led: need %zu, have %u\r\n", sizeof(struct LedConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_SENSOR: // sensor_data - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before sensor\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->sensor_count < 4 && offset + sizeof(struct SensorData) <= configLen - 2) {
                    memcpy(&globalConfig->sensors[globalConfig->sensor_count], &configData[offset], sizeof(struct SensorData));
                    offset += sizeof(struct SensorData);
                    if (offset > configLen) {
                        printf("Offset overflow after sensor\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->sensor_count++;
                } else if (globalConfig->sensor_count >= 4) {
                    offset += sizeof(struct SensorData);
                    if (offset > configLen) {
                        printf("Offset overflow after sensor (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("sensor: need %zu, have %u\r\n", sizeof(struct SensorData), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_DATA_BUS: // data_bus - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before data_bus\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->data_bus_count < 4 && offset + sizeof(struct DataBus) <= configLen - 2) {
                    memcpy(&globalConfig->data_buses[globalConfig->data_bus_count], &configData[offset], sizeof(struct DataBus));
                    offset += sizeof(struct DataBus);
                    if (offset > configLen) {
                        printf("Offset overflow after data_bus\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->data_bus_count++;
                } else if (globalConfig->data_bus_count >= 4) {
                    offset += sizeof(struct DataBus);
                    if (offset > configLen) {
                        printf("Offset overflow after data_bus (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("data_bus: need %zu, have %u\r\n", sizeof(struct DataBus), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;
                
            case CONFIG_PKT_BINARY_INPUT: // binary_inputs - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before binary_input\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->binary_input_count < 4 && offset + sizeof(struct BinaryInputs) <= configLen - 2) {
                    memcpy(&globalConfig->binary_inputs[globalConfig->binary_input_count], &configData[offset], sizeof(struct BinaryInputs));
                    offset += sizeof(struct BinaryInputs);
                    if (offset > configLen) {
                        printf("Offset overflow after binary_input\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->binary_input_count++;
                } else if (globalConfig->binary_input_count >= 4) {
                    offset += sizeof(struct BinaryInputs);
                    if (offset > configLen) {
                        printf("Offset overflow after binary_input (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("binary_input: need %zu, have %u\r\n", sizeof(struct BinaryInputs), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            case CONFIG_PKT_TOUCH: // touch_controller (0x28) - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before touch_controller\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->touch_controller_count < 4 && offset + sizeof(struct TouchController) <= configLen - 2) {
                    memcpy(&globalConfig->touch_controllers[globalConfig->touch_controller_count], &configData[offset], sizeof(struct TouchController));
                    offset += sizeof(struct TouchController);
                    if (offset > configLen) {
                        printf("Offset overflow after touch_controller\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->touch_controller_count++;
                } else if (globalConfig->touch_controller_count >= 4) {
                    offset += sizeof(struct TouchController);
                    if (offset > configLen) {
                        printf("Offset overflow after touch_controller (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("touch_controller: need %zu, have %u\r\n", sizeof(struct TouchController), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            case CONFIG_PKT_PASSIVE_BUZZER: // passive_buzzer (0x29) - parse but don't log
                if (offset > configLen) {
                    printf("Offset overflow before passive_buzzer\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->passive_buzzer_count < 4 && offset + sizeof(struct PassiveBuzzerConfig) <= configLen - 2) {
                    memcpy(&globalConfig->passive_buzzers[globalConfig->passive_buzzer_count], &configData[offset], sizeof(struct PassiveBuzzerConfig));
                    offset += sizeof(struct PassiveBuzzerConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after passive_buzzer\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->passive_buzzer_count++;
                } else if (globalConfig->passive_buzzer_count >= 4) {
                    offset += sizeof(struct PassiveBuzzerConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after passive_buzzer (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("passive_buzzer: need %zu, have %u\r\n", sizeof(struct PassiveBuzzerConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            case CONFIG_PKT_WIFI: // wifi_config (0x26)
                /* The nRF54 radio has no Wi-Fi, but the packet is still parsed and
                 * stored (not skipped) so a client's Wi-Fi settings survive a config
                 * read-back. The old code skipped a hardcoded 162 bytes; the packet
                 * is 160 bytes on the wire, so that off-by-2 desynced every packet
                 * after wifi. */
                if (offset > configLen) {
                    printf("Offset overflow before wifi\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct WifiConfig) <= configLen - 2) {
                    memcpy(&globalConfig->wifi_config, &configData[offset], sizeof(struct WifiConfig));
                    globalConfig->wifi_config_loaded = true;
                    offset += sizeof(struct WifiConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after wifi\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("wifi_config: need %zu, have %u\r\n",
                           sizeof(struct WifiConfig), (unsigned)(configLen - 2 - offset));
                    offset = configLen - 2; // Skip to CRC
                }
                break;

            case CONFIG_PKT_SECURITY: // security_config (0x27)
                if (offset > configLen) {
                    printf("Offset overflow before security_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct SecurityConfig) <= configLen - 2) {
                    memcpy(&s_od_security_parsed, &configData[offset], sizeof(struct SecurityConfig));
                    offset += sizeof(struct SecurityConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after security_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    printf("Security: enabled=%d, flags=0x%02X, reset_pin=%d\r\n",
                                 s_od_security_parsed.encryption_enabled,
                                 s_od_security_parsed.flags,
                                 s_od_security_parsed.reset_pin);
                } else {
                    printf("security_config: need %zu, have %u\r\n",
                                  sizeof(struct SecurityConfig), (unsigned)(configLen - 2 - offset));
                    offset = configLen - 2;
                }
                break;

            case CONFIG_PKT_NFC: // nfc_config (0x2A)
                if (offset > configLen) {
                    printf("Offset overflow before nfc_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->nfc_config_count < 2 && offset + sizeof(struct NfcConfig) <= configLen - 2) {
                    memcpy(&globalConfig->nfc_configs[globalConfig->nfc_config_count], &configData[offset], sizeof(struct NfcConfig));
                    offset += sizeof(struct NfcConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after nfc_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->nfc_config_count++;
                } else if (globalConfig->nfc_config_count >= 2) {
                    offset += sizeof(struct NfcConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after nfc_config (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("nfc_config: need %zu, have %u\r\n", sizeof(struct NfcConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            case CONFIG_PKT_FLASH: // flash_config (0x2B)
                if (offset > configLen) {
                    printf("Offset overflow before flash_config\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (globalConfig->flash_config_count < 2 && offset + sizeof(struct FlashConfig) <= configLen - 2) {
                    memcpy(&globalConfig->flash_configs[globalConfig->flash_config_count], &configData[offset], sizeof(struct FlashConfig));
                    offset += sizeof(struct FlashConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after flash_config\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                    globalConfig->flash_config_count++;
                } else if (globalConfig->flash_config_count >= 2) {
                    offset += sizeof(struct FlashConfig);
                    if (offset > configLen) {
                        printf("Offset overflow after flash_config (skipped)\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("flash_config: need %zu, have %u\r\n", sizeof(struct FlashConfig), (unsigned)(configLen - 2 - offset));
                    globalConfig->loaded = false;
                    return false;
                }
                break;

            case CONFIG_PKT_DATA_EXTENDED:
                if (offset > configLen) {
                    printf("Offset overflow before data_extended\r\n");
                    globalConfig->loaded = false;
                    return false;
                }
                if (offset + sizeof(struct DataExtended) <= configLen - 2) {
                    memcpy(&globalConfig->data_extended, &configData[offset],
                           sizeof(struct DataExtended));
                    data_extended_null_terminate(&globalConfig->data_extended);
                    offset += sizeof(struct DataExtended);
                    globalConfig->data_extended_loaded = true;
                    if (offset > configLen) {
                        printf("Offset overflow after data_extended\r\n");
                        globalConfig->loaded = false;
                        return false;
                    }
                } else {
                    printf("data_extended: need %zu, have %u\r\n",
                           sizeof(struct DataExtended),
                           (unsigned)(configLen - 2 - offset));
                    offset = configLen - 2;
                }
                break;
                
            default: {
                /*
                 * A type with no parse case above (e.g. 0x28 touch / 0x29 buzzer on
                 * this branch). Skip it by its known on-wire size so later packets
                 * are still parsed, instead of jumping to the CRC and silently
                 * dropping everything after it. Only a genuinely unknown type ID
                 * (not in the size table) forces skip-to-CRC, because the TLV format
                 * carries no per-packet length to recover from.
                 */
                uint16_t knownSize = config_packet_data_size(packetId);
                if (knownSize != 0u) {
                    if (offset + knownSize <= configLen - 2) {
                        printf("Known-unparsed pkt 0x%02X @%u, skipping %u B\r\n",
                               packetId, (unsigned)(offset - 2), (unsigned)knownSize);
                        offset += knownSize;
                    } else {
                        printf("Known-unparsed pkt 0x%02X @%u: need %u, have %u\r\n",
                               packetId, (unsigned)(offset - 2), (unsigned)knownSize,
                               (unsigned)(configLen - 2 - offset));
                        offset = configLen - 2; // Truncated packet; stop.
                    }
                } else {
                    printf("Unknown pkt 0x%02X @%u, skip-to-CRC (drops later pkts)\r\n",
                           packetId, (unsigned)(offset - 2));
                    offset = configLen - 2; // Skip to CRC
                }
                break;
            }
        }
    }
    
    printf("Parsed %u pkts, offset=%u/%u\r\n", (unsigned)packetIndex, (unsigned)offset, (unsigned)(configLen - 2));

    rescan_security_packet(configData, configLen);
    if (od_security_key_set() || s_od_security_parsed.encryption_enabled != 0u) {
        printf("Security: enabled=%d, flags=0x%02X, key_set=%d\r\n",
               (int)s_od_security_parsed.encryption_enabled,
               (unsigned)s_od_security_parsed.flags,
               (int)od_security_key_set());
    }
    
    if (configLen >= 2) {
        uint16_t crcGiven = configData[configLen - 2] | (configData[configLen - 1] << 8);
        uint16_t crcCalculated = config_toolbox_outer_crc16(configData, configLen - 2);
        if (crcGiven != crcCalculated) {
            printf("CRC mismatch: 0x%04X vs 0x%04X\r\n", crcGiven, crcCalculated);
        }
    }
    
    globalConfig->loaded = true;
    printf("Config parsed successfully: version=%d, displays=%d, leds=%d, sensors=%d, data_buses=%d, binary_inputs=%d, buzzers=%d, nfc=%d, flash=%d\r\n",
                 globalConfig->version, globalConfig->display_count, globalConfig->led_count,
                 globalConfig->sensor_count, globalConfig->data_bus_count, globalConfig->binary_input_count,
                 globalConfig->passive_buzzer_count, globalConfig->nfc_config_count, globalConfig->flash_config_count);
    return true;
}

bool loadGlobalConfig(struct GlobalConfig* globalConfig) {
    if (globalConfig == NULL) {
        printf("Invalid parameter for loadGlobalConfig\n");
        return false;
    }
    
    memset(globalConfig, 0, sizeof(struct GlobalConfig));
    globalConfig->loaded = false;
    
    static uint8_t configData[MAX_CONFIG_SIZE];
    uint32_t configLen = MAX_CONFIG_SIZE;
    
    if (!initConfigStorage()) {
        printf("Failed to initialize config storage\n");
        return false;
    }
    
    if (!loadConfig(configData, &configLen)) {
        printf("No config found\n");
        return false;
    }
    
    return parseConfigBytes(configData, configLen, globalConfig);
}
