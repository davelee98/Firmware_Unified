/* WiFi.h -- Arduino WiFi + WiFiServer/WiFiClient over esp_wifi and lwip sockets.
 * TEMPORARY; part of the shim.
 *
 * This backs the LAN transport added in Firmware #124 (SECTION 9 of the wire protocol).
 * Two things about that transport shape the shim:
 *
 *   - The device is the SERVER and the host connects to it, on WifiConfig.server_port
 *     (DIVERGENCE_MATRIX §9.1). So WiFiServer/WiFiClient carry the real traffic here and
 *     WiFi.begin() is only the join.
 *   - LAN is plain TCP framed [len:2 LE][payload], with TLS-PSK on server_port + 1. Nothing
 *     in this shim touches framing -- that is core logic and stays in wifi_service.cpp.
 *
 * The destination is od_hal_radio (docs/SHARED_API_DESIGN.md), which is transport-agnostic:
 * `od_hal_radio_send(origin, frame, len)` with BLE and LAN as origins. These sockets become
 * the LAN arm of that, and the Arduino class shapes disappear.
 */

#pragma once

#include "arduino_compat.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "esp_wifi.h"
#include "esp_netif.h"

#define WL_IDLE_STATUS      0
#define WL_NO_SSID_AVAIL    1
#define WL_CONNECTED        3
#define WL_CONNECT_FAILED   4
#define WL_DISCONNECTED     6

class IPAddress {
public:
    IPAddress() {}
    explicit IPAddress(uint32_t addr) : _addr(addr) {}
    String toString() const
    {
        char b[16];
        snprintf(b, sizeof b, "%u.%u.%u.%u",
                 (unsigned)(_addr & 0xFF), (unsigned)((_addr >> 8) & 0xFF),
                 (unsigned)((_addr >> 16) & 0xFF), (unsigned)((_addr >> 24) & 0xFF));
        return String(b);
    }
    operator uint32_t() const { return _addr; }
private:
    uint32_t _addr = 0;
};

/* A connected peer. Owns its socket fd and closes it on stop(). Deliberately shallow: the
 * transport reads and writes whole frames, so there is no buffering to model here. */
class WiFiClient {
public:
    WiFiClient() {}
    explicit WiFiClient(int fd) : _fd(fd) {}

    operator bool() const { return _fd >= 0; }
    bool connected() const { return _fd >= 0; }

    int available() const
    {
        if (_fd < 0) return 0;
        int n = 0;
        return (lwip_ioctl(_fd, FIONREAD, &n) == 0) ? n : 0;
    }

    int read(uint8_t *buf, size_t n)
    {
        if (_fd < 0) return -1;
        return (int)lwip_recv(_fd, buf, n, 0);
    }

    size_t write(const uint8_t *buf, size_t n)
    {
        if (_fd < 0) return 0;
        int sent = (int)lwip_send(_fd, buf, n, 0);
        return sent > 0 ? (size_t)sent : 0;
    }

    void setNoDelay(bool on)
    {
        if (_fd < 0) return;
        int v = on ? 1 : 0;
        lwip_setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
    }

    void stop()
    {
        if (_fd >= 0) {
            lwip_close(_fd);
            _fd = -1;
        }
    }

    int fd() const { return _fd; }

private:
    int _fd = -1;
};

class WiFiServer {
public:
    explicit WiFiServer(uint16_t port = 0) : _port(port) {}

    void begin(uint16_t port = 0)
    {
        if (port) _port = port;
        if (_listen >= 0) return;

        _listen = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_listen < 0) return;

        int one = 1;
        lwip_setsockopt(_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

        struct sockaddr_in a = {};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons(_port);

        if (lwip_bind(_listen, (struct sockaddr *)&a, sizeof a) != 0 ||
            lwip_listen(_listen, 1) != 0) {
            lwip_close(_listen);
            _listen = -1;
            return;
        }
        /* Non-blocking accept: the caller polls this from its main loop and must never be
         * parked in accept() while a BLE transfer is in flight. */
        int flags = lwip_fcntl(_listen, F_GETFL, 0);
        lwip_fcntl(_listen, F_SETFL, flags | O_NONBLOCK);
    }

    void end()
    {
        if (_listen >= 0) {
            lwip_close(_listen);
            _listen = -1;
        }
    }

    void setNoDelay(bool on) { _noDelay = on; }

    WiFiClient available()
    {
        if (_listen < 0) return WiFiClient();
        int fd = lwip_accept(_listen, nullptr, nullptr);
        if (fd < 0) return WiFiClient();
        if (_noDelay) {
            int v = 1;
            lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
        }
        return WiFiClient(fd);
    }

private:
    uint16_t _port = 0;
    int _listen = -1;
    bool _noDelay = false;
};

class WiFiClass {
public:
    int begin(const char *ssid, const char *pass);
    void disconnect(bool wifioff = false);
    int status() const;
    IPAddress localIP() const;
    int32_t RSSI() const;
    int32_t channel() const;
    String BSSIDstr() const;
    void setTxPower(int) {}
    void setSortMethod(int) {}
    void mode(int) {}
};

extern WiFiClass WiFi;
