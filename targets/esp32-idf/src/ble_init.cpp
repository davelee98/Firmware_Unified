#include "ble_init.h"
#include "structs.h"
#include "encryption.h"
#include "display_service.h"
#include "od_log.h"

#ifdef TARGET_NRF
#include <bluefruit.h>
extern "C" {
#include "nrf_soc.h"
}
extern BLEDfu bledfu;
extern BLEService imageService;
extern BLECharacteristic imageCharacteristic;
extern struct GlobalConfig globalConfig;
void connect_callback(uint16_t conn_handle);
void disconnect_callback(uint16_t conn_handle, uint8_t reason);
void imageDataWritten(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len);
String getChipIdHex();
#endif

#ifdef TARGET_ESP32
// NimBLE-Arduino + BLE* aliases arrive via ble_init.h (included above).
String getChipIdHex();
#include "esp32_ble_callbacks.h"

extern struct GlobalConfig globalConfig;
extern BLEServer* pServer;
/* OD: removed with the NimBLE-Arduino port -- extern BLEService* pService; */
extern BLECharacteristic* pTxCharacteristic;
extern BLECharacteristic* pRxCharacteristic;
/* OD: removed with the NimBLE-Arduino port -- extern BLEAdvertisementData* advertisementData; */
/* OD: the NimBLE-Arduino callback objects are gone; GAP/GATT events are handled natively
 * in ble/od_ble_nimble.cpp. */
#endif

#ifdef TARGET_NRF
static uint32_t s_nrf_adv_boost_until = 0;

static constexpr uint16_t NRF_ADV_INTERVAL_MIN = 256;   // 160 ms
static constexpr uint16_t NRF_ADV_INTERVAL_MAX = 1600;  // 1000 ms
static constexpr uint16_t NRF_ADV_BOOST_MIN = 32;         // 20 ms
static constexpr uint16_t NRF_ADV_BOOST_MAX = 48;         // 30 ms
static constexpr uint32_t NRF_ADV_BOOST_MS = 3000;

void ble_nrf_boost_advertising(void) {
    s_nrf_adv_boost_until = millis() + NRF_ADV_BOOST_MS;
}

void ble_nrf_apply_adv_interval(void) {
    if (s_nrf_adv_boost_until != 0 && millis() < s_nrf_adv_boost_until) {
        Bluefruit.Advertising.setInterval(NRF_ADV_BOOST_MIN, NRF_ADV_BOOST_MAX);
    } else {
        s_nrf_adv_boost_until = 0;
        Bluefruit.Advertising.setInterval(NRF_ADV_INTERVAL_MIN, NRF_ADV_INTERVAL_MAX);
    }
}

void ble_nrf_advertising_tick(void) {
    static bool was_boosted = false;
    const bool boosting = (s_nrf_adv_boost_until != 0 && millis() < s_nrf_adv_boost_until);
    if (boosting) {
        was_boosted = true;
        return;
    }
    if (!was_boosted || !Bluefruit.Advertising.isRunning()) {
        was_boosted = false;
        s_nrf_adv_boost_until = 0;
        return;
    }
    was_boosted = false;
    s_nrf_adv_boost_until = 0;
    Bluefruit.Advertising.setInterval(NRF_ADV_INTERVAL_MIN, NRF_ADV_INTERVAL_MAX);
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.start(0);
}

// --- Link-layer diagnostics -------------------------------------------------
// DLE (Data Length Extension) sets the max Link-Layer PDU payload: 27 octets by
// default, up to 251 once negotiated. The nRF peripheral only auto-accepts the
// central's request, which arrives AFTER connect_callback, so we log twice: once
// at connect (baseline) and once ~2.5 s later (negotiated).
void ble_nrf_log_link_params(uint16_t conn_handle, const char* phase) {
    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    if (conn == nullptr) {
        od_log_debug("[LINK %s] no connection (handle %u)", phase, conn_handle);
        return;
    }
    uint8_t  phy = conn->getPHY();
    uint16_t mtu = conn->getMtu();                // ATT MTU (23 default; 247 cap here)
    uint16_t dle = conn->getDataLength();         // LL PDU payload octets (27 default; 251 max)
    uint16_t ci  = conn->getConnectionInterval(); // units of 1.25 ms
    const char* phyStr = (phy == BLE_GAP_PHY_2MBPS) ? "2M" :
                         (phy == BLE_GAP_PHY_1MBPS) ? "1M" : "?";
    od_log_debug("[LINK %s] PHY=%s  ATT_MTU=%u  DLE=%u octets  connInterval=%.2f ms",
                 phase, phyStr, mtu, dle, ci * 1.25f);
}

// One-shot timer (armed only in connect_callback — no per-loop polling). Fires
// once on the FreeRTOS timer task after the central finishes negotiation.
static SoftwareTimer s_link_diag_timer;
static uint16_t      s_link_diag_conn = BLE_CONN_HANDLE_INVALID;

static void ble_nrf_link_diag_cb(TimerHandle_t /*xTimer*/) {
    if (Bluefruit.connected()) {
        ble_nrf_log_link_params(s_link_diag_conn, "negotiated");
    }
}

// Proactively upgrade the link for throughput: the nRF peripheral only auto-accepts
// the central's PHY/DLE requests, so if the phone never asks we stay at 1M / 27 octets.
// Requesting here (both are no-ops if the peer already negotiated the same or better).
void ble_nrf_request_fast_link(uint16_t conn_handle) {
    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    if (conn == nullptr) return;

    // 2 Mbps PHY (tx + rx). Peer may decline and stay at 1M.
    conn->requestPHY(BLE_GAP_PHY_2MBPS);

    // 251-octet Link-Layer PDUs (max DLE). AUTO time lets the controller derive the
    // PHY-appropriate on-air duration.
    ble_gap_data_length_params_t dl;
    dl.max_tx_octets  = 251;
    dl.max_rx_octets  = 251;
    dl.max_tx_time_us = BLE_GAP_DATA_LENGTH_AUTO;
    dl.max_rx_time_us = BLE_GAP_DATA_LENGTH_AUTO;
    ble_gap_data_length_limitation_t limit = { 0, 0, 0 };
    if (!conn->requestDataLengthUpdate(&dl, &limit)) {
        od_log_warn("DLE 251 request rejected (tx_lim=%u rx_lim=%u time_lim_us=%u)",
                    limit.tx_payload_limited_octets, limit.rx_payload_limited_octets,
                    limit.tx_rx_time_limited_us);
    }
    od_log_debug("Requested fast link: 2M PHY + 251-octet DLE");
}

void ble_nrf_arm_link_diag(uint16_t conn_handle) {
    s_link_diag_conn = conn_handle;
    static bool created = false;
    if (!created) {
        // Create the one-shot (repeating=false) on the first connection only.
        s_link_diag_timer.begin(500, ble_nrf_link_diag_cb, NULL, false);
        created = true;
    }
    s_link_diag_timer.reset();   // start/restart the one-shot from now; fires ~2.5 s later
}

void ble_nrf_stack_init() {
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.autoConnLed(false);
    Bluefruit.setTxPower(globalConfig.power_option.tx_power);
    Bluefruit.begin(1, 0);
    od_log_info("BLE initialized successfully");
    od_log_info("Setting up BLE service 0x2446...");
    imageService.begin();
    od_log_info("BLE service started");
    imageCharacteristic.setWriteCallback(imageDataWritten);
    od_log_info("BLE write callback set");
    imageCharacteristic.begin();
    od_log_info("BLE characteristic started");
    // Register the DFU service LAST so its presence/absence (it is only added when
    // encryption is disabled) never shifts the handles of imageCharacteristic and its
    // CCCD. GATT handles are assigned in begin() order; keeping the app characteristic
    // ahead of the conditional DFU service keeps its handles stable across encryption
    // on/off, so a client's cached CCCD handle stays valid and notify setup won't fail
    // with ATT "Invalid handle". Must stay after Bluefruit.begin() (SoftDevice up first).
    if (!isEncryptionEnabled()) {
        bledfu.begin();
        od_log_info("BLE DFU initialized successfully (encryption disabled)");
    } else {
        od_log_info("BLE DFU service NOT initialized (encryption enabled - use CMD_ENTER_DFU)");
    }
    Bluefruit.Periph.setConnectCallback(connect_callback);
    Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
    od_log_info("BLE callbacks registered");
    String deviceName = "OD" + getChipIdHex();
    Bluefruit.setName(deviceName.c_str());
    od_log_info("Device name set to: %s", deviceName.c_str());
    od_log_info("Configuring power management...");
    sd_power_mode_set(NRF_POWER_MODE_LOWPWR);
    sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
    od_log_info("Power management configured");
}

void ble_nrf_advertising_start() {
    od_log_info("Configuring BLE advertising...");
    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addName();
    updatemsdata();
    Bluefruit.Advertising.restartOnDisconnect(true);
    ble_nrf_apply_adv_interval();
    Bluefruit.Advertising.setFastTimeout(10);
    od_log_info("Starting BLE advertising...");
    Bluefruit.Advertising.start(0);
}
#endif

#ifdef TARGET_ESP32
void ble_init() {
    ble_init_esp32(true);
}
#endif

#ifdef TARGET_ESP32
volatile bool bleRestartAdvertisingPending = false;
volatile bool esp32BleNotifySubscribed = false;
volatile bool bleDisconnectCleanupPending = false;
volatile bool msdUpdatePending = false;

/* Rewritten for the NimBLE C API (ble/od_ble.h). The GATT construction that used to live
 * here -- createServer/createService/createCharacteristic/advertising -- is now in
 * ble/od_ble_nimble.cpp, because the NimBLE-Arduino classes it was written against do not
 * exist under ESP-IDF. The GATT layout it builds is byte-for-byte the same; see od_ble.h. */

void esp32_ble_clear_handles(void) {
    od_ble_clear_handles();
}

bool esp32_ble_notify_enabled(void) {
    /* The CCCD is auto-created for NOTIFY and the subscribe event tracks the client's
     * toggle -- same contract as before, now sourced from the C API. */
    return od_ble_notify_enabled();
}

void esp32_restart_ble_advertising(void) {
    bleRestartAdvertisingPending = false;
    od_ble_restart_advertising();
    od_log_info("BLE advertising restarted");
}

void ble_init_esp32(bool update_manufacturer_data) {
    od_log_info("=== Initializing ESP32 BLE ===");
    String deviceName = "OD" + getChipIdHex();
    od_log_info("Device name will be: %s", deviceName.c_str());

    /* Preferred only: the central drives the exchange and may settle lower. */
    od_log_info("Setting preferred BLE ATT MTU to %u...", (unsigned)OD_BLE_PREFERRED_ATT_MTU);
    od_ble_set_preferred_mtu(OD_BLE_PREFERRED_ATT_MTU);

    if (update_manufacturer_data) {
        updatemsdata();
    }

    od_ble_init(deviceName.c_str());

    od_log_info("=== BLE advertising started successfully ===");
    od_log_info("Device ready: %s", deviceName.c_str());
    od_log_info("Waiting for BLE connections...");
}

#endif
