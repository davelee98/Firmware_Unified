/* Wire.h -- Arduino TwoWire over ESP-IDF's i2c_master driver. TEMPORARY; part of the shim.
 *
 * The largest API in the census after String: ~230 call sites across the sensor and touch
 * drivers (sht40, bq27220, touch_input). All of it is the same five-call idiom, which is why
 * a shim is worth writing here rather than rewriting 230 sites by hand during phase B:
 *
 *     Wire.beginTransmission(addr); Wire.write(...); Wire.endTransmission();
 *     Wire.requestFrom(addr, n);    while (Wire.available()) Wire.read();
 *
 * It lands on driver/i2c_master.h (IDF >= 5.2), NEVER the deprecated driver/i2c.h --
 * docs/TOOLCHAINS.md makes that an explicit floor for this target.
 *
 * The real destination is od_hal_i2c (docs/SHARED_API_DESIGN.md), and note what that
 * interface says: the core does not call I2C at all -- sensor and PMIC drivers are target
 * code. So these call sites do not migrate to shared/; they get rewritten against the IDF
 * driver directly when their driver is touched, and this shim disappears with them.
 */

#pragma once

#include "arduino_compat.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

class TwoWire {
public:
    explicit TwoWire(int port = 0) : _port(port) {}

    bool begin(int sda = -1, int scl = -1, uint32_t freq = 100000)
    {
        if (_bus) {
            return true;
        }
        if (sda < 0 || scl < 0) {
            return false;
        }
        _freq = freq ? freq : 100000;

        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port                     = _port;
        cfg.sda_io_num                   = (gpio_num_t)sda;
        cfg.scl_io_num                   = (gpio_num_t)scl;
        cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt            = 7;
        cfg.flags.enable_internal_pullup = true;

        return i2c_new_master_bus(&cfg, &_bus) == ESP_OK;
    }

    void end()
    {
        releaseDevice();
        if (_bus) {
            i2c_del_master_bus(_bus);
            _bus = nullptr;
        }
    }

    /* Arduino lets setClock be called any time; IDF fixes the speed per device, so this only
     * records the value and takes effect on the next device attach. */
    void setClock(uint32_t freq) { _freq = freq ? freq : _freq; }

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

    /* Arduino returns 0 on success and non-zero on error -- the inverse of IDF's esp_err_t
     * convention, and of this repo's own "0 ok, negative on failure". Callers were written
     * against the Arduino sense, so that is what this returns. Getting this backwards would
     * make every I2C error look like success. */
    uint8_t endTransmission(bool = true)
    {
        if (!attach(_addr)) {
            return 4;   /* Arduino: 4 == other error */
        }
        esp_err_t err = i2c_master_transmit(_dev, _tx, _txLen, kTimeoutMs);
        _txLen = 0;
        return (err == ESP_OK) ? 0 : 2;   /* 2 == NACK on address */
    }

    /* Arduino's 3-arg form; the bool is "send a stop", which IDF's transaction API always
     * does, so it is accepted and ignored. */
    size_t requestFrom(uint8_t addr, size_t n, bool) { return requestFrom(addr, n); }

    size_t requestFrom(uint8_t addr, size_t n)
    {
        _rxLen = 0;
        _rxPos = 0;
        if (n > sizeof(_rx) || !attach(addr)) {
            return 0;
        }
        if (i2c_master_receive(_dev, _rx, n, kTimeoutMs) != ESP_OK) {
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
    static constexpr int kTimeoutMs = 100;

    bool attach(uint8_t addr)
    {
        if (!_bus) {
            return false;
        }
        if (_dev && addr == _devAddr) {
            return true;
        }
        releaseDevice();

        i2c_device_config_t dcfg = {};
        dcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dcfg.device_address  = addr;
        dcfg.scl_speed_hz    = _freq;

        if (i2c_master_bus_add_device(_bus, &dcfg, &_dev) != ESP_OK) {
            _dev = nullptr;
            return false;
        }
        _devAddr = addr;
        return true;
    }

    void releaseDevice()
    {
        if (_dev) {
            i2c_master_bus_rm_device(_dev);
            _dev = nullptr;
        }
    }

    int _port;
    uint32_t _freq = 100000;
    i2c_master_bus_handle_t _bus = nullptr;
    i2c_master_dev_handle_t _dev = nullptr;
    uint8_t _devAddr = 0xFF;
    uint8_t _addr = 0;

    /* 64 bytes covers every transfer in these drivers; Arduino's own default is 32. */
    uint8_t _tx[64];
    size_t  _txLen = 0;
    uint8_t _rx[64];
    size_t  _rxLen = 0;
    size_t  _rxPos = 0;
};

extern TwoWire Wire;
