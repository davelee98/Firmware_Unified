/* od_ble -- BLE peripheral for the ESP32 target, on NimBLE's native C API.
 *
 * This replaces NimBLE-Arduino (h2zero), which was a C++ wrapper around the very stack
 * ESP-IDF already ships. No dependency is added by this file: `CONFIG_BT_NIMBLE_ENABLED=y`
 * and the `bt` component were already in the build; only the Arduino-flavoured
 * BLEDevice/BLEServer/BLECharacteristic classes were missing. docs/MIGRATION.md § "The ESP32
 * import is different" names this as one of two pieces that "cannot be shimmed and must be
 * written here".
 *
 * WHAT THIS FILE IS, AND WHAT IT IS NOT
 * -------------------------------------
 * This is the STACK layer: GATT registration, advertising, notify, the connection-management
 * calls, and the raw GAP events. It holds no application policy at all -- no ownership, no
 * instance table, no deferred-work flags, no idea what a "session" is.
 *
 * All of that lives one layer up, in src/ble_transport_esp32.cpp, which implements the
 * portable `BleTransport` seam (src/ble_transport.h) shared with the nRF target. The split is
 * exactly the reference firmware's, with one difference forced by the toolchain: upstream's
 * ble_transport_esp32.cpp talks to NimBLE-Arduino directly and keeps "every NimBLE object as
 * file-static state" inside itself, whereas here the NimBLE C objects stay in this file and
 * the transport reaches them through the `od_ble_*` calls below. Nothing outside ble/ names a
 * NimBLE type, which is the property that comment was protecting.
 *
 * Earlier revisions of this header described a facade for imported Arduino call sites
 * (BLEServer/BLECharacteristic classes in ble_init.h). That facade is GONE: the call sites it
 * existed to keep compiling were themselves replaced when BleTransport landed.
 *
 * THE GATT LAYOUT is preserved exactly, because it is a wire contract with a deployed fleet
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
 * THREADING. NimBLE runs its own host task, so every od_ble_evt_* hook below is called OFF
 * the main loop. The contract the transport must honour there is stated on those hooks and is
 * identical to the reference firmware's: copy bytes, publish the instance entry, attempt the
 * one ownership CAS, set an event flag -- nothing else. The calls in the "driven from the
 * loop task" section below are the mirror image: they issue host-stack operations and must
 * NOT be made from a hook.
 */

#ifndef OD_BLE_H
#define OD_BLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An invalid connection handle, spelled without dragging host/ble_hs.h into callers.
 * Matches BLE_HS_CONN_HANDLE_NONE; static_asserted against it in od_ble_nimble.cpp. */
#define OD_BLE_CONN_NONE 0xFFFFu

/* ------------------------------------------------------------------ lifecycle */

/* Bring up the controller, host, GATT server and advertising. Idempotent.
 * Returns false if the stack could not be brought up, in which case no hook will ever fire.
 * (It used to return void, so a failed nimble_port_init() left the caller believing BLE was
 * live; BleTransport::begin() reports a bool and now has something to report.) */
bool od_ble_init(const char *device_name);

/* Stop the host and RELEASE THE BT CONTROLLER. Call before esp_restart().
 *
 * esp_restart() resets the CPU but not the controller, so without this the next
 * (software-reset) boot re-enters od_ble_init() with a controller that is already enabled;
 * nimble_port_init() then fails and BLE comes up dead for the rest of that boot, with only
 * a log line to say so. Stopping advertising is NOT a substitute -- it does not touch the
 * controller at all. Not needed before deep sleep: that powers the digital core down.
 *
 * Withdraws advertising intent and pumps the controller to quiescence before stopping the
 * host, so nothing is advertising when the controller is released.
 *
 * RETURNS FALSE WHEN THE STACK IS STILL UP. nimble_port_stop() can fail, and this reports that
 * rather than swallowing it: on failure the host task and controller remain allocated and this
 * file's state is left describing reality. A caller that clears its own readiness regardless
 * makes the two layers disagree -- the upper one saying BLE is down while the lower one knows
 * it is not -- which is correctness-review finding F7. Clear caller state only on true. */
bool od_ble_deinit(void);

/* True once the GATT server is registered and the host task is running. */
bool od_ble_is_ready(void);

/* ------------------------------------------------------------------ advertising */

/* od_ble_init() does NOT advertise. This is the only call that starts it, and it is safe to
 * make at any point after init -- including before the host has synced, which it normally
 * will be, since sync completes some milliseconds after init returns while the caller's
 * begin()/startAdvertising() pair are consecutive statements. The request is remembered and
 * applied when an identity address becomes available.
 *
 * od_ble_stop_advertising() withdraws that standing request, so stop means stop: the stack
 * events that re-arm advertising (a failed connection attempt, advertising timeout) will not
 * undo it. The deep-sleep path depends on that. */
void od_ble_restart_advertising(void);
void od_ble_stop_advertising(void);

/* Replace the manufacturer-specific data in the advertisement. Does NOT restart advertising:
 * the payload takes effect on the next od_ble_restart_advertising(). Split that way because
 * restarting under a live connection is a policy question the transport answers, not this
 * layer. `len` is the 16-byte MSD payload from the advert builder. */
void od_ble_set_manufacturer_data(const uint8_t *msd, uint8_t len);

/* Service advertising: publish stack facts into the shared controller and run ONE
 * reconciliation step. Call every loop pass -- the controller decides whether anything is
 * needed, and a caller that tries to decide for it is how two owners appear.
 *
 * start_allowed gates a NEW start only; it never stops a running advertisement. Pass false
 * while an EPD refresh is in progress, so a restart waits for the panel rather than the
 * advertisement being withdrawn mid-refresh.
 *
 * Returns true when this pass made a stack call, for logging. LOOP TASK ONLY -- never from a
 * stack callback; that is the ownership rule the whole design rests on.
 */
bool od_ble_service_advertising(bool start_allowed);

/* Is the radio advertising, as far as the controller that owns it knows? Distinct from "was it
 * requested": od_ble_restart_advertising() records intent and makes no stack call, so ask this
 * only after driving the pump. */
bool od_ble_advertising_active(void);

/* ------------------------------------------------------------------ connections */

/* Number of live connections. This is the stack's peer count, maintained across the
 * connect/disconnect events rather than read back from the host (NimBLE's C API exposes no
 * "how many are attached" accessor the way NimBLEServer::getConnectedCount() does). */
uint8_t od_ble_connected_count(void);

/* Notify ONE connection's TX characteristic.
 *
 * Handle-addressed, deliberately: a broadcast notify (NimBLE-Arduino's two-argument
 * `notify(data, len)`, whose handle defaults to "all subscribed clients") sends every
 * response -- authentication traffic included -- to any second central that connected and
 * subscribed. The transport targets the owner's handle to close that.
 *
 * Returns false when there is no such connection or the stack is out of mbufs. mbuf
 * exhaustion is BACKPRESSURE, not failure: the caller must leave the entry queued. */
bool od_ble_notify_handle(uint16_t conn_handle, const uint8_t *data, uint16_t len);

/* Ask the stack to terminate ONE link, with HCI reason 0x13 (REMOTE_USER_TERMINATED).
 *
 * No reason parameter: 0x13 is the only value this firmware wants and the only one Bluefruit
 * can send at all, so the seam does not offer a choice the other target could not honour.
 * 0x09 (CONN_LIMIT) is NOT legal here -- the controller silently rejects it and the peer stays
 * connected while the code looks like it worked.
 *
 * Returns true if the request was accepted OR the link was already gone (BLE_HS_ENOTCONN /
 * BLE_HS_EALREADY), matching what NimBLE-Arduino's NimBLEServer::disconnect() reports, so a
 * benign race with a client that left first does not read as a failure. A true return means
 * "requested", NOT "down" -- BLE disconnect is asynchronous.
 *
 * Loop task only. */
bool od_ble_disconnect(uint16_t conn_handle);

/* Negotiated connection interval for `conn_handle`, in 1.25 ms units; 0 when unknown or the
 * handle is not connected. The central chooses it; this firmware requests none. */
uint16_t od_ble_conn_interval_units(uint16_t conn_handle);

/* Request 2M PHY + 251-octet DLE on `conn_handle`, to match the nRF target's link tuning.
 * Like nRF, the peripheral only auto-accepts what the central asks for, so without this the
 * link stays at 1M / 27 octets whenever the phone never requests better. Both requests are
 * advisory -- a peer may decline and stay where it is, which is not an error.
 *
 * Loop task only: these are host-stack calls, which the hook contract excludes. */
void od_ble_request_fast_link(uint16_t conn_handle);

/* Read back what was actually negotiated on `conn_handle`. Any out-param may be NULL.
 * PHY values are BLE_GAP_LE_PHY_* (1 = 1M, 2 = 2M, 3 = Coded), 0 when unknown.
 *
 * DLE (max LL PDU octets) is absent, exactly as it is on the reference firmware's ESP32 half:
 * there is no accessor for the negotiated data length, unlike Bluefruit's
 * BLEConnection::getDataLength(). Not an oversight. */
void od_ble_link_params(uint16_t conn_handle, uint8_t *tx_phy_out, uint8_t *rx_phy_out,
                        uint16_t *att_mtu_out, uint16_t *interval_units_out);

/* Preferred ATT MTU, requested at init. Preferred only: the central drives the exchange and
 * may settle lower. */
void od_ble_set_preferred_mtu(uint16_t mtu);

/* The identity address the stack actually advertises, and its type (BLE_ADDR_PUBLIC or
 * BLE_ADDR_RANDOM), in NimBLE's little-endian byte order. Returns false before the host has
 * synced, when there is no address to report.
 *
 * Exists because the LAN mDNS TXT record has to publish the SAME address the advertisement
 * uses. Reading BLE_ADDR_PUBLIC directly is wrong: od_ble_init() resolves its type with
 * ble_hs_id_infer_auto(), which selects a static-random address when no public one is
 * available, and a host that matches on the MAC then cannot correlate BLE with LAN. */
bool od_ble_get_identity_addr(uint8_t addr_out[6], uint8_t *addr_type_out);

/* ------------------------------------------------------------------ events (hooks) */
/*
 * Implemented by src/ble_transport_esp32.cpp; called from the NimBLE HOST TASK.
 *
 * These are plain link-layer facts with no policy attached -- this layer does not know which
 * connection owns the device, and must not. Every one of them runs off the main loop, so an
 * implementation may copy bytes into the RX ring, publish its own instance-table entry,
 * attempt the single ownership claim CAS, and set an event flag. Nothing else: the
 * application work a connect or disconnect implies (updatemsdata()'s I2C poll and shared
 * advertisement vector, EPD force-off with its SPI teardown and rail cut) races loop() and
 * corrupts the heap if run here.
 *
 * They are declared, not weakly defined: a build that links this file must supply them, and
 * a missing hook should be a link error rather than a silently dead BLE stack.
 */

/* A central attached. `conn_handle` is live from this point until od_ble_evt_disconnect. */
void od_ble_evt_connect(uint16_t conn_handle);

/* A central detached. `reason` is NimBLE's, at FULL WIDTH and unmodified -- it spans two
 * ranges, HCI reasons wrapped as BLE_HS_ERR_HCI_BASE + code (0x200 + code) and host-layer
 * BLE_HS_E* codes in 1..31, so it does not fit a uint8_t. Truncating aliases them onto each
 * other: an HCI reason survives by luck (0x213 & 0xFF == 0x13) while BLE_HS_ENOTCONN (7)
 * reads back as the unrelated HCI "memory capacity exceeded". */
void od_ble_evt_disconnect(uint16_t conn_handle, uint16_t reason);

/* A central wrote the CCCD. Per-connection: a contender's subscribe must not be allowed to
 * clear or overwrite the incumbent's apparent notify-readiness. */
void od_ble_evt_subscribe(uint16_t conn_handle, bool enabled);

/* A central wrote the characteristic. `data` points at a buffer owned by this layer and valid
 * only for the duration of the call -- copy what you keep.
 *
 * The payload is BINARY, never a string. Pipe-write frames START with 0x00 (00 70 / 00 71 /
 * 00 81), so any C-string conversion reports length 0 and silently drops every transfer
 * frame. `len` is already bounded by OD_BLE_MAX_FRAME - 3 (253 value bytes): the GATT layer
 * rejects anything larger with ATT 0x0D before this is called. */
void od_ble_evt_write(uint16_t conn_handle, const uint8_t *data, uint16_t len);

/* Negotiation completed for `conn_handle`. `trigger` is a short literal naming what completed
 * ("PHY update", "MTU exchange") -- these arrive asynchronously, well after
 * od_ble_request_fast_link() returns, so they are the only points at which the granted values
 * are knowable. Log-only by contract: no state may be touched here. */
void od_ble_evt_link_negotiated(uint16_t conn_handle, const char *trigger);

#ifdef __cplusplus
}
#endif

#endif /* OD_BLE_H */
