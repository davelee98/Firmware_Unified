#include "config_parser.h"
#include "factory_config.h"
#include "structs.h"
#include "od_log.h"
#include "encryption_state.h"
#include "encryption.h"
#include "power_latch.h"
#include "wifi_service.h"  // OPENDISPLAY_HAS_WIFI + lanActivePort()/lanTlsEnabled()

#include <stdio.h>
#include <string.h>

#ifdef TARGET_NRF
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#endif
#ifdef TARGET_ESP32
/* ESP32: config lives in NVS, not LittleFS (decided 2026-07-25). The three-function seam
 * is od_hal_nvs, shaped per docs/SHARED_API_DESIGN.md so the eventual promotion of this
 * subsystem into shared/core is a repoint rather than a rewrite. */
#include "od_hal_nvs.h"
/* The one network thing this file needs: the STA address, for a status log line. Included
 * under TARGET_ESP32 rather than OPENDISPLAY_HAS_WIFI because the log line itself is not
 * gated on WiFi being compiled in -- on a board without it, the lookup simply returns NULL
 * and the line prints "?". esp_netif is part of IDF regardless. */
#include "esp_netif.h"
#endif

#ifndef COMM_MODE_BLE
#define COMM_MODE_BLE (1 << 0)
#define COMM_MODE_OEPL (1 << 1)
#define COMM_MODE_WIFI (1 << 2)
#endif
#ifndef DEVICE_FLAG_PWR_PIN
#define DEVICE_FLAG_PWR_PIN (1 << 0)
#define DEVICE_FLAG_XIAOINIT (1 << 1)
#define DEVICE_FLAG_WS_PP_INIT (1 << 2)
#define DEVICE_FLAG_BATTERY_LATCH (1 << 3)
#define DEVICE_FLAG_PWR_LATCH_DFF (1 << 4)
#endif

// The parse-time dumps and printConfigSummary() are od_log_debug, which the
// default OD_LOG_LEVEL (INFO) compiles out entirely -- so they no longer cost
// serial time in the deep-sleep wake window and need no runtime quiet flag.

extern struct GlobalConfig globalConfig;
extern uint8_t activeLedInstance;
extern char wifiSsid[33];
extern char wifiPassword[33];
extern uint8_t wifiEncryptionType;
extern bool wifiConfigured;
#ifdef TARGET_ESP32
extern char wifiServerUrl[65];
extern uint16_t wifiServerPort;
// extern bool wifiServerConfigured;  // dead -- see the 0x26 wifi_config parse
extern bool wifiConnected;
extern bool wifiInitialized;
#endif

void xiaoinit();
void powerDownExternalFlashFromConfig(void);
void ws_pp_init();
extern bool encryptionInitialized;

// Defined in main.h (the single-inclusion globals header), so it needs an extern
// here rather than an include -- main.h may not be included twice.
#include "od_config_asm.h"
#include "od_config_tlv.h"
extern struct od_config_asm g_configAsm;

void resetChunkedWriteState(void) {
    /* One line now: the state it clears is shared/core's. Kept as a named primitive because
     * abortToKnownState() and session_guard.cpp call it, and routing every teardown through one
     * function is what stopped three call sites each zeroing a different subset. */
    od_config_asm_reset(&g_configAsm);
}

bool initConfigStorage(){
    #ifdef TARGET_NRF
    if (!InternalFS.begin()) {
        od_log_error("ERROR: Failed to mount internal file system");
        return false;
    }
    return true;
    #endif
    #ifdef TARGET_ESP32
    if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
        od_log_error("ERROR: Failed to initialise NVS config storage");
        return false;
    }
    return true;
    #endif
    return false; // Should never reach here
}

void formatConfigStorage(){
    #ifdef TARGET_NRF
    InternalFS.format();
    #endif
    #ifdef TARGET_ESP32
    (void)od_hal_nvs_erase();
    #endif
}

// See getConfigScratch() in config_parser.h for the sharing contract. Replaces a
// per-consumer 4 KB buffer each in loadGlobalConfig (static), hasValidStoredConfig
// (static) and handleReadConfig (stack -- a 4 KB spike on the loop task).
static uint8_t configScratch[MAX_CONFIG_SIZE];

uint8_t* getConfigScratch(void) {
    return configScratch;
}

bool saveConfig(uint8_t* configData, uint32_t len){
    if (len > MAX_CONFIG_SIZE) {
        od_log_error("ERROR: Config data too large (%u bytes)", (unsigned)len);
        return false;
    }
    if (configData == nullptr) {
        return false;
    }
    // Header on the stack; the payload is written straight from the caller's
    // buffer. Two writes produce the same bytes the old single write did, and
    // the factory-provisioning caller passes a flash pointer, so this also drops
    // a 4 KB flash->RAM copy at first boot.
    config_header_t header;
    header.magic = CONFIG_STORAGE_MAGIC;
    header.version = CONFIG_STORAGE_VERSION;
    header.crc = calculateConfigCRC(configData, len);
    header.data_len = len;
    #ifdef TARGET_ESP32
    /* NVS stores one opaque blob, so header and payload are staged contiguously. The
     * LittleFS path wrote them as two sequential file writes; the bytes on the medium are
     * the same record either way, which keeps loadConfig's validation unchanged.
     *
     * The staging buffer is the cost of the blob interface. It is affordable here -- an
     * ESP32-S3 has 512 KB plus PSRAM -- and it is NOT affordable on the EFR32BG22, whose
     * whole heap is 10.3 KB. When this subsystem is promoted to shared/core, that target
     * will need either a two-key record or a streaming write; do not carry this buffer
     * across as if it were free. See docs/MEMORY_CONSTRAINTS.md.
     */
    static uint8_t blob[sizeof(config_header_t) + MAX_CONFIG_SIZE];
    memcpy(blob, &header, sizeof(header));
    if (len > 0) {
        memcpy(blob + sizeof(header), configData, len);
    }
    if (od_hal_nvs_save(blob, (uint32_t)(sizeof(header) + len)) != OD_HAL_NVS_OK) {
        od_log_error("ERROR: Failed to write config to NVS");
        return false;
    }
    #else
    #ifdef TARGET_NRF
    if (InternalFS.exists(CONFIG_FILE_PATH)) {
        InternalFS.remove(CONFIG_FILE_PATH);
    }
    File file = InternalFS.open(CONFIG_FILE_PATH, FILE_O_WRITE);
    #elif defined(TARGET_ESP32)
    if (LittleFS.exists(CONFIG_FILE_PATH)) {
        LittleFS.remove(CONFIG_FILE_PATH);
    }
    File file = LittleFS.open(CONFIG_FILE_PATH, FILE_WRITE);
    #endif
    if (!file) {
        od_log_error("ERROR: Failed to open config file for writing");
        #ifdef TARGET_NRF
        file = InternalFS.open(CONFIG_FILE_PATH, FILE_O_WRITE);
        #elif defined(TARGET_ESP32)
        file = LittleFS.open(CONFIG_FILE_PATH, FILE_WRITE);
        #endif
        if (!file) {
            od_log_error("ERROR: Failed to open config file for writing with CREATE|WRITE");
        return false;
        }
    }
    const size_t totalSize = sizeof(config_header_t) + len;
    size_t bytesWritten = file.write((uint8_t*)&header, sizeof(header));
    if (bytesWritten == sizeof(header) && len > 0) {
        bytesWritten += file.write(configData, len);
    }
    file.close();
    if (bytesWritten != totalSize) {
        od_log_error("ERROR: Failed to write complete config data (expected %u, wrote %u)", (unsigned)totalSize, (unsigned)bytesWritten);
        return false;
    }
    #endif
    return true;
}

bool clearStoredConfig(void) {
    #ifdef TARGET_NRF
    if (InternalFS.exists(CONFIG_FILE_PATH)) {
        if (!InternalFS.remove(CONFIG_FILE_PATH)) {
            od_log_error("ERROR: Failed to remove config file");
            return false;
        }
    }
    #elif defined(TARGET_ESP32)
    if (od_hal_nvs_erase() != OD_HAL_NVS_OK) {
        od_log_error("ERROR: Failed to remove stored config");
        return false;
    }
    #endif
    memset(&globalConfig, 0, sizeof(globalConfig));
    memset(&securityConfig, 0, sizeof(securityConfig));
    wifiConfigured = false;
    wifiSsid[0] = '\0';
    wifiPassword[0] = '\0';
    wifiEncryptionType = 0;
    return true;
}

bool loadConfig(uint8_t* configData, uint32_t* len){
    if (configData == nullptr || len == nullptr) {
        return false;
    }
    config_header_t header;
#ifdef TARGET_ESP32
    /* One blob out of NVS, then the same validation the LittleFS path applied. Reading the
     * record whole rather than header-then-payload is the one behavioural difference, and it
     * is the safer order: the length check below happens before anything is copied into the
     * caller's buffer, which the sequential-read version could only do after staging. */
    static uint8_t blob[sizeof(config_header_t) + MAX_CONFIG_SIZE];
    uint32_t blobLen = 0;
    int rc = od_hal_nvs_load(blob, (uint32_t)sizeof(blob), &blobLen);
    if (rc == OD_HAL_NVS_ENOENT) {
        return false;           /* unprovisioned device -- not an error, just nothing stored */
    }
    if (rc != OD_HAL_NVS_OK || blobLen < sizeof(config_header_t)) {
        od_log_error("ERROR: Failed to read config from NVS (rc=%d, len=%u)", rc, (unsigned)blobLen);
        return false;
    }
    memcpy(&header, blob, sizeof(header));
    if (header.magic != CONFIG_STORAGE_MAGIC) {
        od_log_error("ERROR: Invalid config magic number");
        return false;
    }
    if (header.data_len > MAX_CONFIG_SIZE) {
        od_log_error("ERROR: Config data too large");
        return false;
    }
    if (header.data_len > *len) {
        od_log_error("ERROR: Config data larger than buffer");
        return false;
    }
    if (blobLen < sizeof(config_header_t) + header.data_len) {
        od_log_error("ERROR: Stored config truncated (header says %u, blob holds %u)",
                     (unsigned)header.data_len, (unsigned)(blobLen - sizeof(config_header_t)));
        return false;
    }
    memcpy(configData, blob + sizeof(config_header_t), header.data_len);
#else
    #ifdef TARGET_NRF
    File file = InternalFS.open(CONFIG_FILE_PATH, FILE_O_READ);
    #elif defined(TARGET_ESP32)
    File file = LittleFS.open(CONFIG_FILE_PATH, FILE_READ);
    #endif
    if (!file) {
        return false;
    }
    if (configData == nullptr || len == nullptr) {
        file.close();
        return false;
    }
    // Header staged on the stack, payload read straight into the caller's buffer.
    // On any failure below configData may hold unvalidated bytes; every caller
    // gates on the return value, so it is never read after a false return.
    config_header_t header;
    size_t bytesRead = file.read((uint8_t*)&header, sizeof(header));
    if (bytesRead != sizeof(header)) {
        od_log_error("ERROR: Failed to read config header (expected %u, got %u)", (unsigned)sizeof(header), (unsigned)bytesRead);
        file.close();
        return false;
    }
    if (header.magic != CONFIG_STORAGE_MAGIC) {
        od_log_error("ERROR: Invalid config magic number");
        file.close();
        return false;
    }
    if (header.data_len > MAX_CONFIG_SIZE) {
        od_log_error("ERROR: Config data too large");
        file.close();
        return false;
    }
    // Capacity is now checked BEFORE the read rather than after staging: the
    // read target is the caller's buffer, so this bound is what keeps a large
    // (but in-spec) config from overrunning a smaller caller buffer.
    if (header.data_len > *len) {
        od_log_error("ERROR: Config data larger than buffer");
        file.close();
        return false;
    }
    bytesRead = file.read(configData, header.data_len);
    file.close();
    if (bytesRead != header.data_len) {
        od_log_error("ERROR: Failed to read complete config data (expected %u, read %u)", (unsigned)header.data_len, (unsigned)bytesRead);
        return false;
    }
#endif
    uint32_t calculatedCRC = calculateConfigCRC(configData, header.data_len);
    if (header.crc != calculatedCRC) {
        od_log_error("ERROR: Config CRC mismatch");
        return false;
    }
    *len = header.data_len;
    return true;
}

bool hasValidStoredConfig(void) {
#ifdef TARGET_NRF
    if (!InternalFS.exists(CONFIG_FILE_PATH)) {
        return false;
    }
#elif defined(TARGET_ESP32)
    /* No presence probe on ESP32. The obvious one -- load into a sizeof(config_header_t)
     * buffer and check for ENOENT -- cannot work: any real record is header + payload, so
     * the HAL returns E2BIG and logs "stored config is N B, buffer is 12 B" at ERROR level
     * on EVERY boot of a perfectly healthy device. The probe never told us anything either,
     * since loadConfig() below already returns false on ENOENT. */
#endif
    uint32_t len = MAX_CONFIG_SIZE;
    return loadConfig(getConfigScratch(), &len);
}

/* The toolbox CRC-16/CCITT that used to live here is now
 * od_config_tlv_crc16() in shared/core. Deleted rather than left beside it: a local copy of a
 * promoted function is dead code that still compiles, and the next edit to one of them is the
 * drift this repo exists to prevent. calculateConfigCRC() below is a DIFFERENT checksum --
 * CRC-32 over the storage record -- and stays. */

uint32_t calculateConfigCRC(uint8_t* data, uint32_t len){
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}

/* One config packet, bounds already guaranteed by od_config_tlv_walk().
 *
 * THE BOUNDS CHECKS ARE GONE FROM HERE, and that is the promotion: this function used to spell
 * out `offset + sizeof(T) <= configLen - 2` once per packet type -- fifteen independent chances
 * to write the wrong comparison on the pre-auth attack surface. That check now exists once, in
 * shared/core/od_config_tlv.c, with host vectors for truncation and overflow.
 *
 * What is left is the part that is genuinely per-target: where each packet is stored, the
 * instance-count caps, and the logging. Returning false aborts the walk; a packet this build
 * does not implement is NOT a reason to do that.
 */
static bool onConfigPacket(void *ctx, uint8_t packetId, od_span_t bodySpan) {
    (void)ctx;
    /* The span is unpacked ONCE, here, and the per-packet arms below stay as they were imported.
     * They are already sizeof-bounded against the same declared size the walk checked, so
     * rewriting thirty memcpy sites would be churn in the one hardware-verified target for no
     * change in what is guaranteed. The bound that matters is upstream: bodySpan.n cannot
     * disagree with bodySpan.p, which is the pairing od_span_t removes. */
    const uint8_t *body = bodySpan.p;
    switch (packetId) {

            case 0x01: // system_config
                {
                    memcpy(&globalConfig.system_config, body, sizeof(struct SystemConfig));
                }
                return true;
            case 0x02: // manufacturer_data
                {
                    memcpy(&globalConfig.manufacturer_data, body, sizeof(struct ManufacturerData));
                }
                return true;
            case 0x04: // power_option
                {
                    memcpy(&globalConfig.power_option, body, sizeof(struct PowerOption));
                }
                return true;
            case 0x20: // display
                if (globalConfig.display_count < 4) {
                    memcpy(&globalConfig.displays[globalConfig.display_count], body, sizeof(struct DisplayConfig));
                    globalConfig.display_count++;
                } else {
                    od_log_warn("WARNING: Maximum display count reached, skipping");
                }
                return true;
            case 0x21: // led
                if (globalConfig.led_count < 4) {
                    memcpy(&globalConfig.leds[globalConfig.led_count], body, sizeof(struct LedConfig));
                    globalConfig.led_count++;
                    // Reset active LED instance to re-detect RGB LEDs after config change
                    activeLedInstance = 0xFF;
                } else {
                    od_log_warn("WARNING: Maximum LED count reached, skipping");
                }
                return true;
            case 0x23: // sensor_data
                if (globalConfig.sensor_count < 4) {
                    memcpy(&globalConfig.sensors[globalConfig.sensor_count], body, sizeof(struct SensorData));
                    globalConfig.sensor_count++;
                } else {
                    od_log_warn("WARNING: Maximum sensor count reached, skipping");
                }
                return true;
            case 0x24: // data_bus
                if (globalConfig.data_bus_count < 4) {
                    memcpy(&globalConfig.data_buses[globalConfig.data_bus_count], body, sizeof(struct DataBus));
                    globalConfig.data_bus_count++;
                } else {
                    od_log_warn("WARNING: Maximum data_bus count reached, skipping");
                }
                return true;
            case 0x25: // binary_inputs
                if (globalConfig.binary_input_count < 4) {
                    memcpy(&globalConfig.binary_inputs[globalConfig.binary_input_count], body, sizeof(struct BinaryInputs));
                    globalConfig.binary_input_count++;
                } else {
                    od_log_warn("WARNING: Maximum binary_input count reached, skipping");
                }
                return true;
            case 0x28: // touch_controller
                if (globalConfig.touch_controller_count < 4) {
                    memcpy(&globalConfig.touch_controllers[globalConfig.touch_controller_count], body, sizeof(struct TouchController));
                    globalConfig.touch_controller_count++;
                } else {
                    od_log_warn("WARNING: Maximum touch_controller count reached, skipping");
                }
                return true;
            case 0x29: // passive_buzzer
                if (globalConfig.passive_buzzer_count < 4) {
                    memcpy(&globalConfig.passive_buzzers[globalConfig.passive_buzzer_count], body, sizeof(struct BuzzerConfig));
                    globalConfig.passive_buzzer_count++;
                } else {
                    od_log_warn("WARNING: Maximum passive_buzzer count reached, skipping");
                }
                return true;
            case 0x2C: // data_extended
                {
                    memcpy(&globalConfig.data_extended, body, sizeof(struct DataExtended));
                    globalConfig.data_extended.manufacturer_name[31] = '\0';
                    globalConfig.data_extended.model_name[31] = '\0';
                    globalConfig.data_extended.serial_number[31] = '\0';
                    globalConfig.data_extended.friendly_name[31] = '\0';
                    globalConfig.data_extended.device_location[31] = '\0';
                    globalConfig.data_extended.device_id[31] = '\0';
                    globalConfig.data_extended.custom_string_1[31] = '\0';
                    globalConfig.data_extended.custom_string_2[31] = '\0';
                    globalConfig.data_extended.custom_string_3[31] = '\0';
                    globalConfig.data_extended_loaded = true;
                }
                return true;
            case 0x2A: // nfc_config -- stored, never acted on (no NFC hardware on this target)
                if (globalConfig.nfc_config_count < 2) {
                    memcpy(&globalConfig.nfc_configs[globalConfig.nfc_config_count], body, sizeof(struct NfcConfig));
                    globalConfig.nfc_config_count++;
                } else {
                    od_log_warn("WARNING: Maximum nfc_config count reached, skipping");
                }
                return true;
            case 0x2B: // flash_config
                if (globalConfig.flash_config_count < 2) {
                    memcpy(&globalConfig.flash_configs[globalConfig.flash_config_count], body, sizeof(struct FlashConfig));
                    globalConfig.flash_config_count++;
                } else {
                    od_log_warn("WARNING: Maximum flash_config count reached, skipping");
                }
                return true;
            case 0x26: // wifi_config (see struct WifiConfig)
                {
                    struct WifiConfig wc;
                    memcpy(&wc, body, sizeof(wc));

                    memcpy(wifiSsid, wc.ssid, sizeof(wc.ssid));
                    wifiSsid[32] = '\0';
                    uint8_t ssidLen = 0;
                    while (ssidLen < 32 && wifiSsid[ssidLen] != '\0') ssidLen++;

                    memcpy(wifiPassword, wc.password, sizeof(wc.password));
                    wifiPassword[32] = '\0';
                    uint8_t passwordLen = 0;
                    while (passwordLen < 32 && wifiPassword[passwordLen] != '\0') passwordLen++;

                    wifiEncryptionType = wc.encryption_type;

#ifdef TARGET_ESP32
                    memcpy(wifiServerUrl, wc.server_host, 64);
                    wifiServerUrl[64] = '\0';

                    bool isStringFormat = false;
                    for (int i = 0; i < 64; i++) {
                        if (wifiServerUrl[i] == '\0') {
                            isStringFormat = true;
                            break;
                        }
                        if (i > 0 && wifiServerUrl[i] < 32 && wifiServerUrl[i] != '\0') {
                            break;
                        }
                    }

                    if (!isStringFormat && wifiServerUrl[4] == '\0' &&
                        (wifiServerUrl[0] != 0 || wifiServerUrl[1] != 0 ||
                         wifiServerUrl[2] != 0 || wifiServerUrl[3] != 0)) {
                        uint8_t ip[4];
                        ip[0] = wc.server_host[0];
                        ip[1] = wc.server_host[1];
                        ip[2] = wc.server_host[2];
                        ip[3] = wc.server_host[3];
                        snprintf(wifiServerUrl, 65, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
                        od_log_debug("Converted numeric IP to string: \"%s\"", wifiServerUrl);
                    } else if (!isStringFormat && wifiServerUrl[0] != '\0') {
                        uint32_t ipNum = (uint32_t)wc.server_host[0] |
                                        ((uint32_t)wc.server_host[1] << 8) |
                                        ((uint32_t)wc.server_host[2] << 16) |
                                        ((uint32_t)wc.server_host[3] << 24);
                        uint8_t ip[4];
                        ip[0] = (ipNum >> 24) & 0xFF;
                        ip[1] = (ipNum >> 16) & 0xFF;
                        ip[2] = (ipNum >> 8) & 0xFF;
                        ip[3] = ipNum & 0xFF;
                        snprintf(wifiServerUrl, 65, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
                        od_log_debug("Converted 32-bit integer to IP string: \"%s\"", wifiServerUrl);
                    }

                    // server_port is the one BIG-ENDIAN field in WifiConfig; read it byte-wise
                    // (former reserved[64]=MSB, reserved[65]=LSB).
                    wifiServerPort = (uint16_t)(((uint16_t)((const uint8_t*)&wc.server_port)[0] << 8) |
                                                ((const uint8_t*)&wc.server_port)[1]);
                    if (wifiServerPort == 0) {
                        wifiServerPort = 2446;
                    }

                    // wifiServerConfigured is dead: it was only ever read by the log
                    // lines that used to sit here. It described the old "tag pushes to
                    // an upload server" model, but the LAN transport inverted that --
                    // the device listens and the host connects to it, so server_host
                    // gates nothing. server_host stays part of the 0x26 wire format.
                    //
                    // Report the endpoint the LAN listener will actually bind, not just
                    // the raw config field: the TLS-PSK channel runs on server_port + 1
                    // and there is no config entry for it.
#ifdef OPENDISPLAY_HAS_WIFI
                    od_log_debug("LAN: %s on port %u (server_port %u)",
                                 lanTlsEnabled() ? "TLS-PSK" : "plaintext",
                                 (unsigned)lanActivePort(), (unsigned)wifiServerPort);
#else
                    od_log_debug("LAN: transport not compiled in (server_port %u)", (unsigned)wifiServerPort);
#endif
#endif
                    wifiConfigured = true;
                    od_log_info("=== WiFi Configuration Loaded ===");
                    // Do NOT log the SSID or password (credentials). Presence/length only.
                    od_log_debug("SSID: (set, %u chars)", ssidLen);
                    od_log_debug("Password: %s", passwordLen > 0 ? "(set)" : "(empty)");
                    const char* encTypeStr = "Unknown";
                    switch (wifiEncryptionType) {
                        case 0x00: encTypeStr = "None (Open)"; break;
                        case 0x01: encTypeStr = "WEP"; break;
                        case 0x02: encTypeStr = "WPA"; break;
                        case 0x03: encTypeStr = "WPA2"; break;
                        case 0x04: encTypeStr = "WPA3"; break;
                    }
                    od_log_debug("Encryption Type: 0x%02X (%s)", wifiEncryptionType, encTypeStr);
                    od_log_debug("SSID length: %u bytes", ssidLen);
                    od_log_debug("Password length: %u bytes", passwordLen);
                    od_log_debug("WiFi configured: true");
                }
                return true;
            case 0x27: // security_config
                {
                    {
                        memcpy(&securityConfig, body, sizeof(struct SecurityConfig));
                        // Check if key is all zeros (encryption disabled)
                        bool keyIsZero = true;
                        for (int i = 0; i < 16; i++) {
                            if (securityConfig.encryption_key[i] != 0) {
                                keyIsZero = false;
                                break;
                            }
                        }
                        if (keyIsZero) {
                            securityConfig.encryption_enabled = 0;
                            od_log_debug("Security config: Encryption disabled (key is all zeros)");
                        } else if (securityConfig.encryption_enabled) {
                            od_log_debug("Security config: Encryption enabled");
                            od_log_debug("Session timeout: %u seconds", securityConfig.session_timeout_seconds);
                        } else {
                            od_log_debug("Security config: Encryption disabled (flag set to 0)");
                        }
                        // Log security flags
                        if (securityConfig.flags & OD_SECURITY_FLAG_REWRITE_ALLOWED) {
                            od_log_debug("Security config: Rewrite allowed (unauthorized config writes permitted)");
                        }
                        if (securityConfig.flags & OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN) {
                            od_log_debug("Security config: Show key on screen enabled (future feature)");
                        }
                        if (securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) {
                            od_log_debug("Security config: Reset pin %u enabled (polarity: %s, pullup: %s, pulldown: %s)",
                                       securityConfig.reset_pin,
                                       (securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_POLARITY) ? "HIGH" : "LOW",
                                       (securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_PULLUP) ? "yes" : "no",
                                       (securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_PULLDOWN) ? "yes" : "no");
                        } else {
                            od_log_debug("Security config: Reset pin disabled");
                        }
                    }
                }
                return true;
            default:
                /* Unreachable: od_config_tlv_walk() ends the walk on an id it does not know,
                 * so an unknown one never reaches this callback. Handled rather than assumed. */
                return true;
    }
}

bool loadGlobalConfig(){
    memset(&globalConfig, 0, sizeof(globalConfig));
    // Initialize security config defaults
    memset(&securityConfig, 0, sizeof(securityConfig));
    // Reset pin defaults to disabled (flag not set)
    wifiConfigured = false;
    wifiSsid[0] = '\0';
    wifiPassword[0] = '\0';
    wifiEncryptionType = 0;
    globalConfig.data_extended_loaded = false;
    uint8_t* configData = getConfigScratch();
    uint32_t configLen = MAX_CONFIG_SIZE;
    if (!loadConfig(configData, &configLen)) {
        globalConfig.loaded = false;
        return false;
    }
    if (configLen < 3) {
        od_log_error("ERROR: Config too short");
        globalConfig.loaded = false;
        return false;
    }
    /* The walk itself is shared. version comes back through it; the per-packet copies happen
     * in onConfigPacket() above. */
    uint8_t parsedVersion = 0;
    uint8_t stoppedOnId = 0;
    const enum od_config_tlv_result walk =
        od_config_tlv_walk(od_span_make(configData, configLen), onConfigPacket, NULL,
                           &parsedVersion, &stoppedOnId);
    if (stoppedOnId != 0) {
        /* Restores the per-packet parser's "Unknown packet ID 0x%02X, skipping" warning. The
         * walk cannot log -- shared/ has no log seam -- so it reports the id and the target
         * says it. Losing this in the promotion would have traded a diagnostic for nothing. */
        od_log_warn("WARNING: Unknown packet ID 0x%02X, remainder of config skipped",
                    stoppedOnId);
    }
    globalConfig.version = parsedVersion;
    globalConfig.minor_version = 0;   // not stored in the current format
    if (walk == OD_CFG_TLV_TOO_SHORT) {
        od_log_error("ERROR: Config too short");
        globalConfig.loaded = false;
        return false;
    }
    if (walk == OD_CFG_TLV_TRUNCATED) {
        /* A packet claimed more bytes than the blob holds. Previously reported per packet type
         * as "Not enough data for <name>"; the walk cannot name the type, so the id is logged
         * instead -- the same information, from one place. */
        od_log_error("ERROR: Config truncated -- a packet claims more data than the blob holds");
        globalConfig.loaded = false;
        return false;
    }

    // Advisory (warn-only) validation using CRC-16/CCITT to match the toolbox, nRF and
    // Silabs firmware. Not enforced: a mismatch logs a warning only.
    //
    // The guard used to be `if (offset < configLen - 2)`, which never fired for a config
    // that parsed cleanly: the loop above only exits once offset has REACHED configLen - 2
    // (or overshot it), so the condition was false for every well-formed config and true
    // only for one the parser had already decided was truncated. The check therefore ran
    // exclusively on inputs it could not validate. Same defect in Firmware/src (and so in
    // the shipped fleet) -- see docs/FOLLOWUPS.md for the upstream propagation.
    if (configLen >= 2) {
        uint16_t crcGiven = configData[configLen - 2] | (configData[configLen - 1] << 8);
        uint16_t crcCalculated = od_config_tlv_crc16(od_span_make(configData, configLen - 2));
        if (crcGiven != crcCalculated) {
            od_log_warn("WARNING: Config CRC mismatch (given: 0x%04X, calculated: 0x%04X)", crcGiven, crcCalculated);
        }
    }
    globalConfig.loaded = true;
    return true;
}

void printConfigSummary(){
    if (!globalConfig.loaded) {
        od_log_debug("Config not loaded");
        return;
    }
    od_log_debug("=== Configuration Summary ===");
    od_log_debug("Version: %u.%u", globalConfig.version, globalConfig.minor_version);
    od_log_debug("Loaded: %s", globalConfig.loaded ? "Yes" : "No");
    od_log_debug(" ");
    od_log_debug("--- System Configuration ---");
    od_log_debug("IC Type: 0x%04X", globalConfig.system_config.ic_type);
    od_log_debug("Communication Modes: 0x%02X", globalConfig.system_config.communication_modes);
    od_log_debug("  BLE: %s", (globalConfig.system_config.communication_modes & COMM_MODE_BLE) ? "enabled" : "disabled");
    od_log_debug("  OEPL: %s", (globalConfig.system_config.communication_modes & COMM_MODE_OEPL) ? "enabled" : "disabled");
    od_log_debug("  WiFi: %s", (globalConfig.system_config.communication_modes & COMM_MODE_WIFI) ? "enabled" : "disabled");
    #ifdef TARGET_ESP32
    if (globalConfig.system_config.communication_modes & COMM_MODE_WIFI) {
        if (wifiConfigured) {
            od_log_debug("  WiFi SSID: (configured)");  // credential; not logged verbatim
            if (wifiInitialized) {
                if (wifiConnected) {
                    // Was WiFi.localIP().toString(). esp_netif directly: the Arduino
                    // IPAddress/String round-trip existed only to format four bytes, and this
                    // file is a config parser -- the pre-auth attack surface heading for
                    // shared/core, where neither type may appear.
                    char ipStr[16] = "?";
                    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    esp_netif_ip_info_t ipInfo;
                    if (sta != NULL && esp_netif_get_ip_info(sta, &ipInfo) == ESP_OK) {
                        snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&ipInfo.ip));
                    }
                    od_log_debug("  WiFi Status: Connected (IP: %s)", ipStr);
                } else {
                    od_log_debug("  WiFi Status: Disconnected");
                }
            } else {
                od_log_debug("  WiFi Status: Not initialized");
            }
        } else {
            od_log_debug("  WiFi Status: Configured but not loaded");
        }
    }
    #endif
    od_log_debug("Device Flags: 0x%02X", globalConfig.system_config.device_flags);
    od_log_debug("  PWR_PIN flag: %s", (globalConfig.system_config.device_flags & DEVICE_FLAG_PWR_PIN) ? "enabled" : "disabled");
    #ifdef TARGET_NRF
    od_log_debug("  XIAOINIT flag: %s", (globalConfig.system_config.device_flags & DEVICE_FLAG_XIAOINIT) ? "enabled" : "disabled");
    #endif
    od_log_debug("  WS_PP_INIT flag: %s", (globalConfig.system_config.device_flags & DEVICE_FLAG_WS_PP_INIT) ? "enabled" : "disabled");
    od_log_debug("  BATTERY_LATCH flag: %s", (globalConfig.system_config.device_flags & DEVICE_FLAG_BATTERY_LATCH) ? "enabled" : "disabled");
    od_log_debug("  PWR_LATCH_DFF flag: %s", (globalConfig.system_config.device_flags & DEVICE_FLAG_PWR_LATCH_DFF) ? "enabled" : "disabled");
    od_log_debug("Power Pin: %u", globalConfig.system_config.pwr_pin);
    od_log_debug("Power Pin 2: %u", globalConfig.system_config.pwr_pin_2);
    od_log_debug("Power Pin 3: %u", globalConfig.system_config.pwr_pin_3);
    od_log_debug(" ");
    od_log_debug("--- Manufacturer Data ---");
    od_log_debug("Manufacturer ID: 0x%04X", globalConfig.manufacturer_data.manufacturer_id);
    od_log_debug("Board Type: %u", globalConfig.manufacturer_data.board_type);
    od_log_debug("Board Revision: %u", globalConfig.manufacturer_data.board_revision);
    od_log_debug(" ");
    od_log_debug("--- Power Configuration ---");
    od_log_debug("Power Mode: %u", globalConfig.power_option.power_mode);
    od_log_debug("Battery Capacity: %u %u %u mAh",
               globalConfig.power_option.battery_capacity_mah[0],
               globalConfig.power_option.battery_capacity_mah[1],
               globalConfig.power_option.battery_capacity_mah[2]);
    od_log_debug("Awake Timeout: %u ms", globalConfig.power_option.sleep_timeout_ms);
    od_log_debug("Deep Sleep Time: %u seconds", globalConfig.power_option.deep_sleep_time_seconds);
    od_log_debug("Min Wake Time: %u seconds", globalConfig.power_option.min_wake_time_seconds);
    od_log_debug("TX Power: %u", globalConfig.power_option.tx_power);
    od_log_debug("Sleep Flags: 0x%02X", globalConfig.power_option.sleep_flags);
    od_log_debug("Button Wake: %s (sleep_flags bit0)", (globalConfig.power_option.sleep_flags & OD_SLEEP_FLAG_BUTTON_WAKE_DISABLE) ? "disabled" : "enabled");
    od_log_debug("Screen Timeout: %u s (EPD keep-alive; 0 = off immediately after refresh)", globalConfig.power_option.screen_timeout_seconds);
    od_log_debug("Battery Sense Pin: %u", globalConfig.power_option.battery_sense_pin);
    od_log_debug("Battery Sense Enable Pin: %u", globalConfig.power_option.battery_sense_enable_pin);
    od_log_debug("Battery Sense Flags: 0x%02X", globalConfig.power_option.battery_sense_flags);
    od_log_debug("  ENABLE_INVERTED: %s", (globalConfig.power_option.battery_sense_flags & OD_BATTERY_SENSE_FLAG_ENABLE_INVERTED) ? "yes" : "no");
    od_log_debug("Capacity Estimator: %u", globalConfig.power_option.capacity_estimator);
    od_log_debug("Voltage Scaling Factor: %u", globalConfig.power_option.voltage_scaling_factor);
    od_log_debug("Deep Sleep Current: %u uA", (unsigned)globalConfig.power_option.deep_sleep_current_ua);
    od_log_debug(" ");
    od_log_debug("--- Display Configurations (%u) ---", globalConfig.display_count);
    for (int i = 0; i < globalConfig.display_count; i++) {
        od_log_debug("Display %d:", i);
        od_log_debug("  Instance: %u", globalConfig.displays[i].instance_number);
        od_log_debug("  Technology: 0x%02X", globalConfig.displays[i].display_technology);
        od_log_debug("  Panel IC Type: 0x%04X", globalConfig.displays[i].panel_ic_type);
        od_log_debug("  Resolution: %ux%u", globalConfig.displays[i].pixel_width, globalConfig.displays[i].pixel_height);
        od_log_debug("  Size: %ux%u mm", globalConfig.displays[i].active_width_mm, globalConfig.displays[i].active_height_mm);
        od_log_debug("  Tag Type: 0x%04X", globalConfig.displays[i].legacy_tag_type);
        od_log_debug("  Rotation: %u degrees", (unsigned)(globalConfig.displays[i].rotation * 90));
        od_log_debug("  Reset Pin: %u", globalConfig.displays[i].reset_pin);
        od_log_debug("  Busy Pin: %u", globalConfig.displays[i].busy_pin);
        od_log_debug("  DC Pin: %u", globalConfig.displays[i].dc_pin);
        od_log_debug("  CS Pin: %u", globalConfig.displays[i].cs_pin);
        od_log_debug("  Data Pin: %u", globalConfig.displays[i].data_pin);
        od_log_debug("  Partial Update: %s", globalConfig.displays[i].partial_update_support ? "Yes" : "No");
        od_log_debug("  Color Scheme: 0x%02X", globalConfig.displays[i].color_scheme);
        od_log_debug("  Transmission Modes: 0x%02X", globalConfig.displays[i].transmission_modes);
        od_log_debug("    ZIPXL: %s", (globalConfig.displays[i].transmission_modes & OD_TRANSMISSION_MODE_STREAMING_DECOMPRESSION) ? "enabled" : "disabled");
        od_log_debug("    ZIP: %s", (globalConfig.displays[i].transmission_modes & OD_TRANSMISSION_MODE_ZIP) ? "enabled" : "disabled");
        od_log_debug("    G5: %s", (globalConfig.displays[i].transmission_modes & OD_TRANSMISSION_MODE_G5) ? "enabled" : "disabled");
        od_log_debug("    DIRECT_WRITE: %s", (globalConfig.displays[i].transmission_modes & OD_TRANSMISSION_MODE_DIRECT_WRITE) ? "enabled" : "disabled");
        od_log_debug("    CLEAR_ON_BOOT: %s", (globalConfig.displays[i].transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT) ? "enabled" : "disabled");
        od_log_debug("  Full update energy (mC): %u", globalConfig.displays[i].full_update_mC);
        od_log_debug(" ");
    }
    od_log_debug("--- LED Configurations (%u) ---", globalConfig.led_count);
    for (int i = 0; i < globalConfig.led_count; i++) {
        od_log_debug("LED %d:", i);
        od_log_debug("  Instance: %u", globalConfig.leds[i].instance_number);
        od_log_debug("  Type: 0x%02X", globalConfig.leds[i].led_type);
        od_log_debug("  Pins: R=%u G=%u B=%u 4=%u",
                   globalConfig.leds[i].led_1_r,
                   globalConfig.leds[i].led_2_g,
                   globalConfig.leds[i].led_3_b,
                   globalConfig.leds[i].led_4);
        od_log_debug("  Flags: 0x%02X", globalConfig.leds[i].led_flags);
        od_log_debug(" ");
    }
    od_log_debug("--- Sensor Configurations (%u) ---", globalConfig.sensor_count);
    for (int i = 0; i < globalConfig.sensor_count; i++) {
        od_log_debug("Sensor %d:", i);
        od_log_debug("  Instance: %u", globalConfig.sensors[i].instance_number);
        od_log_debug("  Type: 0x%04X", globalConfig.sensors[i].sensor_type);
        od_log_debug("  Bus ID: %u", globalConfig.sensors[i].bus_id);
        od_log_debug("  I2C addr (7-bit) / MSD data start byte: %u / %u", globalConfig.sensors[i].i2c_addr_7bit, globalConfig.sensors[i].msd_data_start_byte);
        od_log_debug(" ");
    }
    od_log_debug("--- Data Bus Configurations (%u) ---", globalConfig.data_bus_count);
    for (int i = 0; i < globalConfig.data_bus_count; i++) {
        od_log_debug("Data Bus %d:", i);
        od_log_debug("  Instance: %u", globalConfig.data_buses[i].instance_number);
        od_log_debug("  Type: 0x%02X", globalConfig.data_buses[i].bus_type);
        od_log_debug("  Pins: 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u",
                   globalConfig.data_buses[i].pin_1,
                   globalConfig.data_buses[i].pin_2,
                   globalConfig.data_buses[i].pin_3,
                   globalConfig.data_buses[i].pin_4,
                   globalConfig.data_buses[i].pin_5,
                   globalConfig.data_buses[i].pin_6,
                   globalConfig.data_buses[i].pin_7);
        od_log_debug("  Speed: %u Hz", (unsigned)globalConfig.data_buses[i].bus_speed_hz);
        od_log_debug("  Flags: 0x%02X", globalConfig.data_buses[i].bus_flags);
        od_log_debug("  Pullups: 0x%02X", globalConfig.data_buses[i].pullups);
        od_log_debug("  Pulldowns: 0x%02X", globalConfig.data_buses[i].pulldowns);
        od_log_debug(" ");
    }
    od_log_debug("--- Binary Input Configurations (%u) ---", globalConfig.binary_input_count);
    for (int i = 0; i < globalConfig.binary_input_count; i++) {
        od_log_debug("Binary Input %d:", i);
        od_log_debug("  Instance: %u", globalConfig.binary_inputs[i].instance_number);
        od_log_debug("  Type: 0x%02X", globalConfig.binary_inputs[i].input_type);
        od_log_debug("  Display As: 0x%02X", globalConfig.binary_inputs[i].display_as);
        od_log_debug("  Pins: 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u 8=%u",
                   globalConfig.binary_inputs[i].input_pin_1,
                   globalConfig.binary_inputs[i].input_pin_2,
                   globalConfig.binary_inputs[i].input_pin_3,
                   globalConfig.binary_inputs[i].input_pin_4,
                   globalConfig.binary_inputs[i].input_pin_5,
                   globalConfig.binary_inputs[i].input_pin_6,
                   globalConfig.binary_inputs[i].input_pin_7,
                   globalConfig.binary_inputs[i].input_pin_8);
        od_log_debug("  Input Flags: 0x%02X", globalConfig.binary_inputs[i].pins_used);
        od_log_debug("  Invert: 0x%02X", globalConfig.binary_inputs[i].invert);
        od_log_debug("  Pullups: 0x%02X", globalConfig.binary_inputs[i].pullups);
        od_log_debug("  Pulldowns: 0x%02X", globalConfig.binary_inputs[i].pulldowns);
        if (globalConfig.binary_inputs[i].input_type == 3) {
            od_log_debug("  ADC Ladder: count=%u idBase=%u byteIdx=%u",
                        globalConfig.binary_inputs[i].reserved[0],
                        globalConfig.binary_inputs[i].reserved[1],
                        globalConfig.binary_inputs[i].button_data_byte_index);
        }
        od_log_debug(" ");
    }
    od_log_debug("--- Touch Controllers (%u) ---", globalConfig.touch_controller_count);
    for (int i = 0; i < globalConfig.touch_controller_count; i++) {
        od_log_debug("Touch %d:", i);
        od_log_debug("  Instance: %u", globalConfig.touch_controllers[i].instance_number);
        od_log_debug("  IC type: %u", globalConfig.touch_controllers[i].touch_ic_type);
        od_log_debug("  Bus ID: %u", globalConfig.touch_controllers[i].bus_id);
        od_log_debug("  I2C addr (7-bit): 0x%02X", globalConfig.touch_controllers[i].i2c_addr_7bit);
        od_log_debug("  INT/RST/EN pins: %u / %u / %u",
                    globalConfig.touch_controllers[i].int_pin,
                    globalConfig.touch_controllers[i].rst_pin,
                    globalConfig.touch_controllers[i].enable_pin);
        od_log_debug("  Display instance: %u", globalConfig.touch_controllers[i].display_instance);
        od_log_debug("  Flags: 0x%02X", globalConfig.touch_controllers[i].flags);
        od_log_debug("  Poll ms / MSD start byte: %u / %u", globalConfig.touch_controllers[i].poll_interval_ms, globalConfig.touch_controllers[i].touch_data_start_byte);
        od_log_debug(" ");
    }
    od_log_debug("--- Passive buzzers (%u) ---", globalConfig.passive_buzzer_count);
    for (int i = 0; i < globalConfig.passive_buzzer_count; i++) {
        od_log_debug("Buzzer %d:", i);
        od_log_debug("  Instance: %u", globalConfig.passive_buzzers[i].instance_number);
        od_log_debug("  Drive / enable pin: %u / %u", globalConfig.passive_buzzers[i].drive_pin, globalConfig.passive_buzzers[i].enable_pin);
        od_log_debug("  Flags: 0x%02X", globalConfig.passive_buzzers[i].flags);
        od_log_debug("  Duty %%: %u", globalConfig.passive_buzzers[i].duty_percent);
        od_log_debug(" ");
    }
    if (globalConfig.data_extended_loaded) {
        od_log_debug("--- Data Extended ---");
        od_log_debug("  manufacturer_name: %s", (char*)globalConfig.data_extended.manufacturer_name);
        od_log_debug("  model_name: %s",        (char*)globalConfig.data_extended.model_name);
        od_log_debug("  serial_number: %s",     (char*)globalConfig.data_extended.serial_number);
        od_log_debug("  friendly_name: %s",     (char*)globalConfig.data_extended.friendly_name);
        od_log_debug("  device_location: %s",   (char*)globalConfig.data_extended.device_location);
        od_log_debug("  device_id: %s",         (char*)globalConfig.data_extended.device_id);
        od_log_debug("  custom_string_1: %s",   (char*)globalConfig.data_extended.custom_string_1);
        od_log_debug("  custom_string_2: %s",   (char*)globalConfig.data_extended.custom_string_2);
        od_log_debug("  custom_string_3: %s",   (char*)globalConfig.data_extended.custom_string_3);
        od_log_debug(" ");
    }
    od_log_debug("=============================");
}

void full_config_init() {
    od_log_info("Initializing config storage...");
    if (!initConfigStorage()) {
        od_log_error("Config storage initialization failed");
        return;
    }
    od_log_info("Config storage initialized successfully");

#ifdef FACTORY_CLEAR_CONFIG_ON_BOOT
    od_log_info("Factory clear build: erasing stored config");
    clearStoredConfig();
    od_log_info("Config cleared; skipping load");
    return;
#endif

    od_log_info("Loading global configuration...");
    bool configLoaded = loadGlobalConfig();
    if (!configLoaded && tryProvisionFactoryEmbed()) {
        configLoaded = loadGlobalConfig();
    }
    if (configLoaded) {
        od_log_info("Global configuration loaded successfully");
        printConfigSummary();
        clearEncryptionSession();
        encryptionInitialized = true;
        checkResetPin();
#ifdef TARGET_NRF
        powerDownExternalFlashFromConfig();
        if (globalConfig.loaded && (globalConfig.system_config.device_flags & DEVICE_FLAG_XIAOINIT)) {
            od_log_info("Device flag DEVICE_FLAG_XIAOINIT is set, calling xiaoinit()...");
            xiaoinit();
            od_log_info("xiaoinit() completed");
        }
#endif
        if (globalConfig.loaded && (globalConfig.system_config.device_flags & DEVICE_FLAG_WS_PP_INIT)) {
            od_log_info("Device flag DEVICE_FLAG_WS_PP_INIT is set, calling ws_pp_init()...");
            ws_pp_init();
            od_log_info("ws_pp_init() completed");
        }
        // Must run after config load: latch pins/flag come from globalConfig.
        powerLatchBegin();
    } else {
        od_log_error("Global configuration load failed or no config found");
    }
}
