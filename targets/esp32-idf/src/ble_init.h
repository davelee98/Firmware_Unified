#ifndef BLE_INIT_H
#define BLE_INIT_H

#include <stdint.h>

#ifdef TARGET_NRF
void ble_nrf_stack_init();
void ble_nrf_advertising_start();
void ble_nrf_boost_advertising(void);
void ble_nrf_apply_adv_interval(void);
void ble_nrf_advertising_tick(void);
// Link diagnostics: log negotiated PHY / ATT MTU / DLE (LL PDU octets) / conn interval.
void ble_nrf_log_link_params(uint16_t conn_handle, const char* phase);
void ble_nrf_arm_link_diag(uint16_t conn_handle);   // one-shot: re-log ~2.5 s after connect
void ble_nrf_request_fast_link(uint16_t conn_handle); // request 2M PHY + 251-octet DLE (max throughput)
#endif
#ifdef TARGET_ESP32
/* ESP32 BLE runs on NimBLE's NATIVE C API (ble/od_ble.h), not NimBLE-Arduino.
 *
 * The classes below are a deliberately tiny facade -- three methods, no state -- so the
 * ~15 existing call sites in main.cpp and device_control.cpp keep compiling unmodified.
 * Phase B's job is to link and boot, not to rewrite call sites; those get rewritten when
 * communication.cpp is promoted to shared/core, and this facade dies with them.
 *
 * It is NOT a portability layer and must not grow. If something needs a fourth method,
 * that is a signal to call od_ble_* directly instead.
 */
#include "od_ble.h"

class BLECharacteristic {
public:
    bool notify(const uint8_t *data, uint16_t len) { return od_ble_notify(data, len); }
};

class BLEAdvertising {
public:
    void stop()  { od_ble_stop_advertising(); }
    void start() { od_ble_restart_advertising(); }
};

class BLEServer {
public:
    uint8_t getConnectedCount() { return od_ble_connected_count(); }
    BLEAdvertising *getAdvertising() { static BLEAdvertising a; return &a; }
};

void ble_init();
void ble_init_esp32(bool update_manufacturer_data = true);
void esp32_restart_ble_advertising(void);
void esp32_ble_clear_handles(void);
bool esp32_ble_notify_enabled(void);
extern volatile bool bleRestartAdvertisingPending;
extern volatile bool esp32BleNotifySubscribed;
// Set flag-only by MyBLEServerCallbacks (NimBLE host task); serviced from loop()
// so the heavyweight teardown / I2C+advertisement work never races loop().
extern volatile bool bleDisconnectCleanupPending;
extern volatile bool msdUpdatePending;
#endif

#endif
