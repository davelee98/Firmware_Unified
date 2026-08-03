/* HardwareSerial.h -- Arduino HardwareSerial over IDF's UART driver. TEMPORARY; part of the
 * shim.
 *
 * Exactly one user: main.cpp's OPENDISPLAY_LOG_UART path, which puts the console on a
 * separate UART instead of USB-CDC. The reTerminal-class boards (*-extuart) need it because
 * their console is an onboard CH343P USB-UART on GPIO43/44, not native USB.
 *
 * It was missing until those boards were added -- no board fragment defined
 * OPENDISPLAY_LOG_UART, so the `#include <HardwareSerial.h>` inside that guard was never
 * reached and the gap was invisible.
 *
 * Only what main.cpp uses is provided: construct on a UART number, begin() with an explicit
 * pin pair, and the Stream write path that od_log_init() consumes. The destination is
 * od_hal_log -- a single `void od_hal_log(const char *line)` (docs/SHARED_API_DESIGN.md) --
 * so this does not migrate to shared/; it dies with the shim.
 */
#pragma once

#include "arduino_compat.h"
#include "driver/uart.h"
#include "driver/gpio.h"

/* Arduino's frame-format constants. Only 8N1 is used; the value is Arduino's own encoding,
 * kept so a call site copied from the source repo still reads correctly. */
#define SERIAL_8N1 0x800001c

class HardwareSerial : public Stream {
public:
    explicit HardwareSerial(int uart_num) : _uart((uart_port_t)uart_num) {}

    /* Arduino's 5-arg form. `config` is accepted and ignored: the only value any call site
     * passes is SERIAL_8N1, which is what this configures. Decoding Arduino's bit layout to
     * re-derive a setting nothing varies would be more code and more to get wrong. */
    void begin(unsigned long baud, uint32_t config = SERIAL_8N1,
               int8_t rxPin = -1, int8_t txPin = -1)
    {
        (void)config;
        if (_open) {
            return;
        }
        uart_config_t cfg = {};
        cfg.baud_rate = (int)baud;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity    = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        if (uart_param_config(_uart, &cfg) != ESP_OK) {
            return;
        }
        if (uart_set_pin(_uart, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            return;
        }
        /* TX-only ring buffer: this is a log sink, nothing reads from it. 0 for the RX buffer
         * is not allowed by the driver, so it gets the minimum. */
        if (uart_driver_install(_uart, kMinRxBuf, kTxBuf, 0, NULL, 0) != ESP_OK) {
            return;
        }
        _open = true;
    }

    void end()
    {
        if (_open) {
            uart_driver_delete(_uart);
            _open = false;
        }
    }

    size_t write(const uint8_t *b, size_t n) override
    {
        if (!_open || !b || n == 0) {
            return 0;
        }
        int w = uart_write_bytes(_uart, (const char *)b, n);
        return w > 0 ? (size_t)w : 0;
    }

    /* Real free space in the TX ring, non-blocking -- od_log's off-loop backoff polls this and
     * drops the record when it does not come up, so a made-up constant would either stall the
     * producer forever or defeat the backoff entirely. A closed port reports 0, which is
     * correct: nothing can be written to it, and od_log then discards rather than spins. */
    int availableForWrite() override
    {
        size_t free_bytes = 0;
        if (!_open || uart_get_tx_buffer_free_size(_uart, &free_bytes) != ESP_OK) {
            return 0;
        }
        return (int)free_bytes;
    }

    void flush()
    {
        if (_open) {
            uart_wait_tx_done(_uart, pdMS_TO_TICKS(100));
        }
    }

    operator bool() const { return _open; }

private:
    static constexpr int kTxBuf     = 2048;
    static constexpr int kMinRxBuf  = 256;   /* driver rejects 0 */

    uart_port_t _uart;
    bool _open = false;
};
