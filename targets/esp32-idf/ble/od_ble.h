/* od_ble -- BLE peripheral for the ESP32 target, on NimBLE's native C API.
 *
 * This replaces NimBLE-Arduino (h2zero), which was a C++ wrapper around the very stack
 * ESP-IDF already ships. No dependency is added by this file: `CONFIG_BT_NIMBLE_ENABLED=y`
 * and the `bt` component were already in the build; only the Arduino-flavoured
 * BLEDevice/BLEServer/BLECharacteristic classes were missing. docs/MIGRATION.md § "The ESP32
 * import is different" names this as one of two pieces that "cannot be shimmed and must be
 * written here".
 *
 * The GATT layout is preserved exactly, because it is a wire contract with a deployed fleet
 * and with py-opendisplay:
 *
 *   service        0000 2446 -0000-1000-8000-00805F9B34FB
 *   characteristic 0000 2446 -0000-1000-8000-00805F9B34FB   (the SAME UUID)
 *                  properties READ | NOTIFY | WRITE | WRITE_NR
 *                  CCCD 0x2902 auto-created for NOTIFY
 *
 * TX and RX are one characteristic: the host writes commands to it and subscribes to
 * notifications on it. Do not "tidy" that into two.
 *
 * Threading. NimBLE runs its own host task, so every callback here arrives OFF the main
 * loop. The imported design is already correct about this and is preserved: callbacks SET
 * FLAGS ONLY (msdUpdatePending, bleDisconnectCleanupPending, bleRestartAdvertisingPending)
 * and loop() does the heavyweight work. The comments in the original callbacks explain why --
 * updatemsdata() drives I2C and mutates the shared advertisement vector, and disconnect
 * teardown cuts the panel rail; doing either on the host task races loop() and corrupts the
 * heap. That constraint is the same one shared/core's od_core_rx/od_core_process split
 * exists to express (docs/SHARED_API_DESIGN.md).
 */

#ifndef OD_BLE_H
#define OD_BLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the controller, host, GATT server and advertising. Idempotent. */
void od_ble_init(const char *device_name);

/* Notify the TX characteristic. Returns false if there is no connection or no subscriber --
 * the same contract the caller's `pTxCharacteristic->notify()` had. */
bool od_ble_notify(const uint8_t *data, uint16_t len);

/* Number of live connections (0 or 1 -- CONFIG_BT_NIMBLE_MAX_CONNECTIONS is 1). */
uint8_t od_ble_connected_count(void);

/* True when a client has written the CCCD to enable notifications. */
bool od_ble_notify_enabled(void);

/* Replace the manufacturer-specific data in the advertisement and restart advertising so it
 * takes effect. `len` is the 16-byte MSD payload from the advert builder. */
void od_ble_set_manufacturer_data(const uint8_t *msd, uint8_t len);

/* Stop then start advertising. Safe to call when already advertising. */
void od_ble_restart_advertising(void);

void od_ble_stop_advertising(void);

/* Drop the GATT handles (used on teardown paths that previously nulled the C++ pointers). */
void od_ble_clear_handles(void);

/* Preferred ATT MTU, requested at init. */
void od_ble_set_preferred_mtu(uint16_t mtu);

/* Stop the host and RELEASE THE BT CONTROLLER. Call before esp_restart().
 *
 * esp_restart() resets the CPU but not the controller, so without this the next
 * (software-reset) boot re-enters od_ble_init() with a controller that is already enabled;
 * nimble_port_init() then fails and BLE comes up dead for the rest of that boot, with only
 * a log line to say so. Stopping advertising is NOT a substitute -- it does not touch the
 * controller at all. Not needed before deep sleep: that powers the digital core down. */
void od_ble_deinit(void);

/* The identity address the stack actually advertises, and its type (BLE_ADDR_PUBLIC or
 * BLE_ADDR_RANDOM), in NimBLE's little-endian byte order. Returns false before the host has
 * synced, when there is no address to report.
 *
 * Exists because the LAN mDNS TXT record has to publish the SAME address the advertisement
 * uses. Reading BLE_ADDR_PUBLIC directly is wrong: od_ble_init() resolves its type with
 * ble_hs_id_infer_auto(), which selects a static-random address when no public one is
 * available, and a host that matches on the MAC then cannot correlate BLE with LAN. */
bool od_ble_get_identity_addr(uint8_t addr_out[6], uint8_t *addr_type_out);

#ifdef __cplusplus
}
#endif

#endif /* OD_BLE_H */
