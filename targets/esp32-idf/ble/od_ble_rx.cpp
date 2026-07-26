/* od_ble_on_write -- the RX half of the BLE characteristic.
 *
 * Called from the NimBLE host task by ble/od_ble_nimble.cpp's GATT write handler. It does
 * exactly what MyBLECharacteristicCallbacks::onWrite did: bounds-check and ENQUEUE, nothing
 * more. The parsing, decryption and dispatch happen on the loop task when it drains the
 * queue -- which is the same split shared/core formalises as od_core_rx / od_core_process
 * (docs/SHARED_API_DESIGN.md), and the reason it exists is the same: this runs off the main
 * loop and must not touch panel, I2C or transfer state.
 *
 * The payload is BINARY. The original carried a warning worth preserving: converting it to a
 * string truncates at the first 0x00, and pipe-write frames START with 0x00 (00 70 / 00 71 /
 * 00 81), so a string conversion reports length 0 and silently drops every transfer frame.
 */
/* NOT main.h: that header DEFINES its globals rather than declaring them, so including it
 * from a second translation unit duplicates every one at link time. It was only ever
 * included by main.cpp. esp32_ble_callbacks.h carries the extern declarations and the queue
 * type, which is all this file needs. */
#include <string.h>
#include "esp32_ble_callbacks.h"
#include "od_log.h"

/* C++ linkage: od_ble_nimble.cpp declares this without extern "C", so the definition must
 * mangle the same way. */
void od_ble_on_write(const uint8_t *data, uint16_t len)
{
    if (data == nullptr || len == 0) {
        od_log_warn("WARNING: Empty data received");
        return;
    }
    if (len > MAX_COMMAND_SIZE) {
        od_log_warn("WARNING: Command too large, dropping");
        return;
    }

    /* One-line RX log, mirroring the "[BLE] TX ..." response log and gated by the same
     * mid-stream quiet check the Arduino callback used. The port dropped this; it is the
     * single most useful line on the console while bringing a board up, because it is the
     * only evidence that a write reached the application at all. */
    if (!imageWriteLogQuietFrame(data, len)) {
        uint16_t cmd = (len >= 2) ? (uint16_t)((data[0] << 8) | data[1]) : data[0];
        char line[160] = {0};
        int pos = snprintf(line, sizeof(line), "BLE: RX 0x%04X (%u B):", cmd, (unsigned)len);
        if (pos < 0) {
            pos = 0;
            line[0] = '\0';
        }
        int dumpLen = (len < 32) ? (int)len : 32;
        for (int i = 0; i < dumpLen && pos < (int)sizeof(line); i++) {
            int n = snprintf(line + pos, sizeof(line) - pos, " %02X", data[i]);
            if (n < 0) {
                break;
            }
            pos += n;
        }
        if (len > 32 && pos < (int)sizeof(line)) {
            snprintf(line + pos, sizeof(line) - pos, " ...");
        }
        od_log_debug("%s", line);
    }

    uint8_t head     = __atomic_load_n(&commandQueueHead, __ATOMIC_RELAXED);
    uint8_t tail     = __atomic_load_n(&commandQueueTail, __ATOMIC_ACQUIRE);
    uint8_t nextHead = (head + 1) % COMMAND_QUEUE_SIZE;

    if (nextHead == tail) {
        od_log_error("ERROR: Command queue full, dropping command");
        return;
    }
    memcpy(commandQueue[head].data, data, len);
    commandQueue[head].len = len;
    commandQueue[head].pending = true;
    __atomic_store_n(&commandQueueHead, nextHead, __ATOMIC_RELEASE);
}
