#ifndef BLE_TRANSPORT_ESP32_H
#define BLE_TRANSPORT_ESP32_H

// Stack-facing declarations for the ESP32 BleTransport implementation.
// Included ONLY from ble_transport_esp32.cpp, inside that file's TARGET_ESP32 gate.
//
// This is what stops stack types leaking. The BLE* aliases that used to live in ble_init.h
// were included by six translation units (main.h, communication.cpp, display_service.cpp,
// device_control.cpp, wifi_service.cpp, esp32_ble_callbacks.h); application code now includes
// ble_transport.h only, and that header names no stack type at all.
//
// WHAT CHANGED FROM THE REFERENCE FIRMWARE. Upstream this header holds `#include
// <NimBLEDevice.h>` plus a block of `using BLEServer = NimBLEServer;`-style aliases, because
// its implementation drives NimBLE-Arduino (h2zero) directly. Under ESP-IDF that library does
// not exist: BLE runs on NimBLE's own C API, wrapped by ../ble/od_ble.h. So the aliases are
// gone -- there are no C++ stack classes to alias -- and this header exists to name the one
// include that replaces them. It is deliberately not merged into the .cpp: keeping the file
// preserves the reference tree's include graph, so the next sync from Firmware diffs cleanly
// instead of showing a deleted file and an unexplained new include.
#ifdef TARGET_ESP32

#include "od_ble.h"

#endif  // TARGET_ESP32
#endif  // BLE_TRANSPORT_ESP32_H
