/* Wire.h -- Arduino TwoWire over hal/od_hal_i2c. TEMPORARY; part of the shim.
 *
 * WHAT CHANGED IN PHASE C STEP 5: the bus mechanics moved to hal/od_hal_i2c.{h,c} and this
 * became a thin adapter. Nothing about the bus behaviour changed -- the IDF calls, the
 * timeouts, the device-handle cache and both hard-won fixes below are the same code, in one
 * place instead of two.
 *
 * That move was FORCED, not cosmetic. IDF permits exactly one i2c_new_master_bus() per port.
 * The sensor drivers converting to od_hal_i2c while display_service.cpp and touch_input.cpp
 * still drive Wire would have meant two owners of one bus, which does not fail cleanly: it
 * fails wherever the two disagree about who configured the pins. One owner, one adapter.
 *
 * ~230 call sites across the sensor and touch drivers, all the same five-call idiom:
 *
 *     Wire.beginTransmission(addr); Wire.write(...); Wire.endTransmission();
 *     Wire.requestFrom(addr, n);    while (Wire.available()) Wire.read();
 *
 * The remaining users are touch_input.cpp (phase C step 6) and display_service.cpp (step 10,
 * the AXP2101 PMIC and the bus lifecycle). This file disappears with them; the sensor drivers
 * already left.
 *
 * TWO BEHAVIOURS THAT LOOK LIKE DETAIL AND ARE NOT. Both were found on hardware, both now live
 * in od_hal_i2c, and both are documented here because this is where the Arduino semantics they
 * implement are visible:
 *
 *   * A ZERO-LENGTH WRITE IS A PRESENCE PROBE. `beginTransmission(a); endTransmission();` with
 *     no write() between is the universal Arduino "is anything at this address?", and
 *     sensor_sht40.cpp's bus scan was exactly that. IDF's i2c_master_transmit() rejects size 0
 *     outright, so nothing reached the bus -- no START, no address byte, no ACK to observe --
 *     and mapping that refusal to "NACK on address" made every probe of every address report
 *     absent, identically whether or not hardware was there. A connected SHT40 was
 *     undetectable. Routed to od_hal_i2c_probe(), which is IDF's address-only primitive.
 *
 *   * endTransmission(false) MUST NOT SEND A STOP. It is the repeated-START register-read
 *     idiom and sensor_bq27220.cpp depends on it. Discarding the flag turns it into
 *     STOP + fresh START, which the BQ27220 does not accept for register reads -- it returns
 *     whatever an unaddressed read yields, so the gauge produces plausible garbage instead of
 *     a clean error. The staged bytes are held and the next requestFrom() emits both halves as
 *     one od_hal_i2c_write_read().
 */

#pragma once

#include "arduino_compat.h"
#include "od_hal_i2c.h"

class TwoWire {
public:
    explicit TwoWire(int port = 0) { (void)port; }

    bool begin(int sda = -1, int scl = -1, uint32_t freq = 100000)
    {
        if (od_hal_i2c_is_up()) {
            return true;
        }
        if (sda < 0 || scl < 0) {
            return false;
        }
        return od_hal_i2c_init((uint8_t)sda, (uint8_t)scl, freq);
    }

    void end() { od_hal_i2c_deinit(); }

    void setClock(uint32_t freq) { od_hal_i2c_set_clock(freq); }

    void beginTransmission(uint8_t addr)
    {
        _addr = addr;
        _txLen = 0;
    }

    size_t write(uint8_t b)
    {
        if (_txLen >= sizeof(_tx)) {
            return 0;   /* Arduino silently drops past its buffer too; match that */
        }
        _tx[_txLen++] = b;
        return 1;
    }

    size_t write(const uint8_t *data, size_t n)
    {
        size_t written = 0;
        for (size_t i = 0; i < n; i++) {
            written += write(data[i]);
        }
        return written;
    }

    /* Arduino's return codes, which callers DO distinguish, and which are the INVERSE of both
     * IDF's esp_err_t convention and od_hal_i2c's:
     *   0 success, 1 data too long, 2 NACK on address, 3 NACK on data, 4 other error.
     * Getting this backwards would make every I2C error look like success, so the translation
     * is here at the Arduino boundary and nowhere else. */
    uint8_t endTransmission(bool sendStop = true)
    {
        if (!sendStop) {
            /* Hold the bytes; requestFrom() emits them with a repeated START. Arduino reports
             * success because nothing has been transmitted yet to fail. */
            if (!od_hal_i2c_is_up()) {
                _txLen = 0;
                return 4;
            }
            _pendingTx = true;
            return 0;
        }
        _pendingTx = false;

        if (_txLen == 0) {
            return mapProbe(od_hal_i2c_probe(_addr));
        }

        const int rc = od_hal_i2c_write(_addr, _tx, (uint16_t)_txLen);
        _txLen = 0;
        return mapWrite(rc);
    }

    /* Arduino's 3-arg form; its bool is "send a STOP after the read", which the IDF transaction
     * API always does. Accepted and ignored -- unlike endTransmission's flag, which is not. */
    size_t requestFrom(uint8_t addr, size_t n, bool) { return requestFrom(addr, n); }

    size_t requestFrom(uint8_t addr, size_t n)
    {
        const bool haveRepeatedStart = _pendingTx && (addr == _addr);
        const size_t txLen = _txLen;
        _pendingTx = false;
        _txLen = 0;
        _rxLen = 0;
        _rxPos = 0;
        if (n == 0 || n > sizeof(_rx)) {
            return 0;
        }
        int rc;
        if (haveRepeatedStart && txLen > 0) {
            rc = od_hal_i2c_write_read(addr, _tx, (uint16_t)txLen, _rx, (uint16_t)n);
        } else {
            rc = od_hal_i2c_read(addr, _rx, (uint16_t)n);
        }
        if (rc != OD_HAL_I2C_OK) {
            return 0;
        }
        _rxLen = n;
        return n;
    }

    int available() const { return (int)(_rxLen - _rxPos); }

    int read()
    {
        if (_rxPos >= _rxLen) {
            return -1;
        }
        return _rx[_rxPos++];
    }

private:
    /* A probe distinguishes "nobody answered" (2) from every other failure (4); the difference
     * is what tells a bus scan from a broken bus. */
    static uint8_t mapProbe(int rc)
    {
        if (rc == OD_HAL_I2C_OK)      return 0;
        if (rc == OD_HAL_I2C_ENODEV)  return 2;
        return 4;
    }

    /* A real transfer that failed: the peer NACKed, or the bus misbehaved. Only the argument
     * error is "other"; everything else reports as an address NACK, matching what this shim
     * reported before the mechanics moved. */
    static uint8_t mapWrite(int rc)
    {
        if (rc == OD_HAL_I2C_OK)     return 0;
        if (rc == OD_HAL_I2C_EINVAL) return 4;
        return 2;
    }

    uint8_t _addr = 0;
    /* Set by endTransmission(false): the staged bytes in _tx are a register selector waiting
     * for the matching requestFrom() to emit them with a repeated START. */
    bool _pendingTx = false;

    /* 64 bytes covers every transfer in these drivers; Arduino's own default is 32. */
    uint8_t _tx[64];
    size_t  _txLen = 0;
    uint8_t _rx[64];
    size_t  _rxLen = 0;
    size_t  _rxPos = 0;
};

extern TwoWire Wire;
