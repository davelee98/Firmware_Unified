/* SPI.h -- Arduino SPI over IDF's spi_master. TEMPORARY; part of the shim.
 *
 * Only the methods the imported sources actually call are provided --
 * begin/beginTransaction/transfer/transfer16/writeBytes/endTransaction/end.
 *
 * HISTORY, because the previous version of this comment caused a real defect: it claimed
 * "the panel SPI does NOT come through here: bb_epaper drives its own bus via its esp_idf
 * backend, which is why this is so small", and on that basis every method was an empty stub
 * (transfer() returned its argument, writeBytes() did nothing). The claim is true of
 * bb_epaper and false of everything else. These are real users:
 *
 *   - third_party/FastEPD/src/FastEPD.inl  -- the whole IT8951 command/data/read protocol
 *   - src/display_fastepd.cpp              -- the IT8951 row blit
 *   - src/display_service.cpp              -- the E1004 dual-CS pixel stream
 *
 * With a stubbed transfer16() the IT8951 handshake reads all zeros and every pixel write is
 * discarded, while the panel's GPIO-driven reset/busy handshake still looks healthy -- a
 * failure that only appears on hardware. bb_epaper genuinely does not come through here; it
 * keeps its own esp_idf backend and its own bus, unchanged.
 *
 * CS is NOT managed here. Every caller drives its own chip-select with gpio_set_level()
 * around the transaction (FastEPD does this because the IT8951 needs CS held across a
 * preamble/argument pair), so the device is registered with spics_io_num = -1.
 *
 * The real destination is od_hal_spi (docs/SHARED_API_DESIGN.md), which the core needs
 * almost nothing from -- the panel path stays target code.
 */
#pragma once
/* Deliberately does NOT include arduino_compat.h. FastEPD's arduino_io.inl defines its own
 * `void delay(uint32_t)` / `delayMicroseconds(uint32_t)`, and the shim declares
 * `void delay(long)` / `delayMicroseconds(long)`; pulling both into one translation unit
 * makes every delay(<int literal>) call in FastEPD.inl ambiguous. Nothing here needs the
 * Arduino surface -- only fixed-width ints and the IDF driver. */
#include <stdint.h>
#include <stddef.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#define MSBFIRST   1
#define LSBFIRST   0
#define SPI_MODE0  0
#define SPI_MODE1  1
#define SPI_MODE2  2
#define SPI_MODE3  3

class SPISettings {
public:
    SPISettings() {}
    SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
        : _clock(clock), _bitOrder(bitOrder), _dataMode(dataMode) {}
    uint32_t _clock = 1000000;
    uint8_t  _bitOrder = MSBFIRST;
    uint8_t  _dataMode = SPI_MODE0;
};

class SPIClass {
public:
    /* Arduino-ESP32 order: begin(sck, miso, mosi, ss). FastEPD calls it exactly that way
     * (FastEPD.inl, initIT8951). ss is ignored -- callers drive CS themselves. */
    void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1, int8_t ss = -1)
    {
        (void)ss;
        if (_bus_ok) {
            return;
        }
        if (sck < 0 || mosi < 0) {
            return;   /* nothing usable to configure */
        }

        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num     = mosi;
        buscfg.miso_io_num     = miso;   /* -1 is valid: write-only bus */
        buscfg.sclk_io_num     = sck;
        buscfg.quadwp_io_num   = -1;
        buscfg.quadhd_io_num   = -1;
        /* A 1872-wide 1bpp row is 234 B; the E1004 chunks at 128 B. 4096 leaves headroom
         * without reserving a large DMA descriptor pool. */
        buscfg.max_transfer_sz = 4096;

        esp_err_t err = spi_bus_initialize(kHost, &buscfg, SPI_DMA_CH_AUTO);
        /* INVALID_STATE == somebody already initialised this host. Treat it as usable
         * rather than failing: a second begin() from a different driver must not take the
         * bus away from the first. */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE("od_spi", "spi_bus_initialize failed: %d", (int)err);
            return;
        }
        _owns_bus = (err == ESP_OK);
        _bus_ok   = true;
    }

    void end()
    {
        releaseDevice();
        if (_bus_ok && _owns_bus) {
            spi_bus_free(kHost);
        }
        _bus_ok   = false;
        _owns_bus = false;
    }

    void beginTransaction(SPISettings s)
    {
        if (!_bus_ok) {
            return;
        }
        if (!attach(s._clock, s._dataMode)) {
            return;
        }
        /* Arduino's beginTransaction is an exclusive-access claim. spi_device_acquire_bus
         * is the IDF equivalent and also lets the polling transmits below skip re-arbitration
         * per word, which matters: the IT8951 sends thousands of 16-bit words per frame. */
        if (spi_device_acquire_bus(_dev, portMAX_DELAY) == ESP_OK) {
            _bus_held = true;
        }
    }

    void endTransaction()
    {
        if (_bus_held) {
            spi_device_release_bus(_dev);
            _bus_held = false;
        }
    }

    uint8_t transfer(uint8_t v)
    {
        if (!_dev) {
            return 0;
        }
        spi_transaction_t t = {};
        t.flags     = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.length    = 8;
        t.tx_data[0] = v;
        if (spi_device_polling_transmit(_dev, &t) != ESP_OK) {
            return 0;
        }
        return t.rx_data[0];
    }

    /* MSB-first 16-bit word: tx_data[0] goes out first, so it carries the high byte. The
     * IT8951 protocol is defined in 16-bit words (0x6000 command preamble, 0x1000 read
     * preamble), which is why this exists separately from transfer(). */
    uint16_t transfer16(uint16_t v)
    {
        if (!_dev) {
            return 0;
        }
        spi_transaction_t t = {};
        t.flags      = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.length     = 16;
        t.tx_data[0] = (uint8_t)(v >> 8);
        t.tx_data[1] = (uint8_t)(v & 0xFF);
        if (spi_device_polling_transmit(_dev, &t) != ESP_OK) {
            return 0;
        }
        return (uint16_t)((t.rx_data[0] << 8) | t.rx_data[1]);
    }

    void writeBytes(const uint8_t *data, uint32_t n)
    {
        if (!_dev || !data || n == 0) {
            return;
        }
        /* Chunked to max_transfer_sz so a caller passing a whole plane cannot exceed the
         * DMA descriptor limit and fail the whole write with one error. */
        const uint32_t kChunk = 4096;
        uint32_t off = 0;
        while (off < n) {
            uint32_t take = n - off;
            if (take > kChunk) take = kChunk;
            spi_transaction_t t = {};
            t.length    = take * 8;
            t.tx_buffer = data + off;
            if (spi_device_polling_transmit(_dev, &t) != ESP_OK) {
                return;
            }
            off += take;
        }
    }

private:
    /* SPI2_HOST is the general-purpose host on every ESP32 variant this target builds for
     * (SPI1 is the flash controller). bb_epaper opens its own device on its own terms. */
    static constexpr spi_host_device_t kHost = SPI2_HOST;

    bool attach(uint32_t clock, uint8_t mode)
    {
        if (_dev && clock == _dev_clock && mode == _dev_mode) {
            return true;
        }
        /* A clock or mode change needs a new device registration -- IDF fixes both per
         * device, unlike Arduino where beginTransaction re-programs the peripheral. */
        releaseDevice();

        spi_device_interface_config_t dcfg = {};
        dcfg.clock_speed_hz = (int)(clock ? clock : 1000000);
        dcfg.mode           = mode & 3;
        dcfg.spics_io_num   = -1;      /* callers drive CS */
        dcfg.queue_size     = 1;       /* polling transmits only */

        if (spi_bus_add_device(kHost, &dcfg, &_dev) != ESP_OK) {
            _dev = nullptr;
            return false;
        }
        _dev_clock = clock;
        _dev_mode  = mode;
        return true;
    }

    void releaseDevice()
    {
        if (_bus_held && _dev) {
            spi_device_release_bus(_dev);
            _bus_held = false;
        }
        if (_dev) {
            spi_bus_remove_device(_dev);
            _dev = nullptr;
        }
    }

    spi_device_handle_t _dev = nullptr;
    uint32_t _dev_clock = 0;
    uint8_t  _dev_mode  = 0xFF;
    bool _bus_ok   = false;
    bool _owns_bus = false;
    bool _bus_held = false;
};

extern SPIClass SPI;
