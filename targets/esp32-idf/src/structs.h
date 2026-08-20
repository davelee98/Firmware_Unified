#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>
// Canonical wire contract: config + message payload structs, OD_-prefixed enums
// and flag macros, and (transitively) opendisplay_protocol.h framing constants
// (CMD_*, RESP_*, PIPE_*, config limits). This header is the single source of
// truth for every config-packet struct; the firmware-only runtime types below
// are the only definitions that remain local.
#include "opendisplay_structs.h"
#include "od_rxq.h"
#include "od_txq.h"
// The parsed-config aggregate, the instance caps and the two storage normalisations are
// shared/core/od_config.h. `struct GlobalConfig` was this file's copy of it.
#include "od_config.h"
#include "od_pipe.h"

#define BOOT_ROW_BUFFER_SIZE 960

// Image transfer state variables
struct ImageData {
    uint8_t* data;
    uint32_t size;
    uint32_t received;
    uint8_t dataType;
    bool isCompressed;
    uint32_t crc32;
    uint16_t width;
    uint16_t height;
    bool ready;
    uint32_t totalBlocks;
    uint32_t currentBlock;
    bool* blocksReceived;
    uint32_t* blockBytesReceived;  // Track bytes received per block
    uint32_t* blockPacketsReceived; // Track packets received per block
};

// LOCAL link policy: the ATT MTU this device asks the stack to negotiate on the
// BLE transport. "Preferred" is literal -- the central drives the exchange and may
// settle lower, so nothing may assume this value was granted. Deliberately
// distinct from OD_BLE_MAX_FRAME: that is the cross-repo whole-ATT-MTU ceiling used to size
// storage buffers, whereas this is one device's request on one physical
// link. They are equal today; the asserts below state the coupling that actually
// matters instead of leaving it to a shared symbol.
//
// Applied by BleTransport::begin() on ESP32 only (NimBLE setMTU ->
// ble_att_set_preferred_mtu). NOT used on nRF: Bluefruit fixes the MTU at
// BLE_GATT_ATT_MTU_MAX (247) via configPrphBandwidth(BANDWIDTH_MAX) ->
// configPrphConn(247, ...) before the SoftDevice starts, and 256 exceeds that cap.
// This is the one BleTransport method whose effect is genuinely target-specific
// rather than merely differently implemented; see both ble_transport_*.cpp.
#define OD_BLE_PREFERRED_ATT_MTU  256u

// A single-PDU write carries OD_BLE_PREFERRED_ATT_MTU - 3 value bytes (ATT opcode 1 +
// handle 2). It must (a) still admit the largest legitimate inbound frame, and (b) never
// exceed the slot the payload is copied into.
static_assert(OD_BLE_PREFERRED_ATT_MTU - 3u >= PIPE_MAX_FRAME,
              "negotiated MTU too small for the largest pipe frame");
static_assert(OD_BLE_PREFERRED_ATT_MTU - 3u <= OD_BLE_MAX_FRAME,
              "a single-PDU write could overrun an OD_BLE_MAX_FRAME-sized slot");

// The BLE RX command ring (od_rxq_item_t and its sizes) lives in shared/core/od_rxq.h; egress is
// shared/core/od_txq.c. Neither is a config-packet or wire-protocol definition, so neither belongs
// in this hub.

#define MAX_BUTTONS 32  // Up to 4 instances * 8 pins = 32 buttons max
struct ButtonState {
    uint8_t button_id;          // Button ID (0-7, from instance_number + pin offset)
    uint8_t press_count;         // Press count (0-15)
    volatile uint8_t current_state;       // Current button state (0=released, 1=pressed, updated in ISR)
    uint8_t byte_index;          // Byte index in dynamicreturndata
    uint8_t pin;                 // GPIO pin number
    uint8_t instance_index;      // BinaryInputs instance index
    bool initialized;          // Whether this button is initialized
    uint8_t pin_offset;         // Pin offset within instance (0-7) for faster ISR lookup
    bool inverted;              // Inverted flag for this pin (cached for ISR)
    bool power_off;             // Whether this button triggers a power-off
    uint16_t power_off_hold_ms; // Hold duration (ms) required to trigger power-off
};

#endif
