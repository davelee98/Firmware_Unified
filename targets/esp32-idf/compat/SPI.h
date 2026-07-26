/* SPI.h -- Arduino SPI over IDF's spi_master. TEMPORARY; part of the shim.
 *
 * Only the handful of methods the imported sources actually call are provided --
 * beginTransaction/endTransaction/transfer/writeBytes/end. The panel SPI does NOT come
 * through here: bb_epaper drives its own bus via its esp_idf backend, which is why this is
 * so small. docs/SHARED_API_DESIGN.md § od_hal_spi says the same thing -- the core needs
 * almost nothing here, and the real panel path is bb_epaper's own IO backend.
 */
#pragma once
#include "arduino_compat.h"
#include "driver/spi_master.h"

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
    uint8_t  _bitOrder = 0;
    uint8_t  _dataMode = 0;
};

class SPIClass {
public:
    void begin(int8_t = -1, int8_t = -1, int8_t = -1, int8_t = -1) {}
    void end() {}
    void beginTransaction(SPISettings) {}
    void endTransaction() {}
    uint8_t transfer(uint8_t v) { return v; }
    uint16_t transfer16(uint16_t v) { return v; }
    void writeBytes(const uint8_t *, uint32_t) {}
};

extern SPIClass SPI;
