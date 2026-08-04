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

#include <errno.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "esp_wifi.h"
#include "esp_netif.h"

#define WL_IDLE_STATUS      0
#define WL_NO_SSID_AVAIL    1
#define WL_CONNECTED        3
#define WL_CONNECT_FAILED   4
#define WL_DISCONNECTED     6



/* Arduino's wl_status_t is an enum; the shim's status() returns int, so alias it. */
typedef int wl_status_t;

/* Arduino's WiFi event plumbing. The sources register a diagnostic handler; IDF uses the
 * default event loop with a different signature, so the handler is not wired up here --
 * it is diagnostics only, and wiring it wrongly would be worse than leaving it silent. */
typedef int arduino_event_id_t;

#define ARDUINO_EVENT_WIFI_STA_CONNECTED     0
#define ARDUINO_EVENT_WIFI_STA_DISCONNECTED  1
#define ARDUINO_EVENT_WIFI_STA_GOT_IP        2

/* The diagnostic handler reads fields off this union. Providing the shape keeps the handler
 * compiling; it is never dispatched (see onEvent below), so the fields stay zero rather than
 * carrying stale values that would read as real diagnostics. */
struct arduino_event_info_t {
    struct { uint8_t bssid[6]; uint8_t channel; } wifi_sta_connected;
    struct { uint8_t reason; } wifi_sta_disconnected;
    struct { struct { struct { uint32_t addr; } ip; } ip_info; } got_ip;
};

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

    /* Arduino's connected() reports the PEER's state, not just "I hold an fd" -- it peeks the
     * socket and returns false once the peer has sent FIN. Returning `_fd >= 0` looked
     * equivalent and is not: on the plain-TCP LAN path available() also returns 0 for a
     * closed socket, so a client that vanished left wifiServerConnected true indefinitely and
     * the session was never reaped. (The TLS path was unaffected: it maps recv == 0 to
     * OD_LAN_READ_CLOSED itself.) */
    bool connected() const
    {
        if (_fd < 0) {
            return false;
        }
        uint8_t b;
        int r = lwip_recv(_fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r > 0) {
            return true;            /* data waiting */
        }
        if (r == 0) {
            return false;           /* orderly shutdown by the peer */
        }
        /* Report DISCONNECTED only on an errno that definitely means it. Everything else --
         * including EWOULDBLOCK (the common "idle but healthy" case) and any errno this lwip
         * build might return for an unsupported flag -- stays connected. Erring the other way
         * would tear down live sessions on a spurious error, which is worse than the leak
         * this check exists to fix. */
        return !(errno == ECONNRESET || errno == ENOTCONN ||
                 errno == EPIPE      || errno == EBADF);
    }

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

    /* Arduino's setTimeout is in ms and applies to blocking reads. Mapped to SO_RCVTIMEO,
     * which is the closest lwip equivalent -- note it bounds a single recv(), not a whole
     * frame read, so a caller relying on it to bound framing needs its own deadline. */
    void setTimeout(uint32_t ms)
    {
        if (_fd < 0) return;
        struct timeval tv;
        tv.tv_sec  = (time_t)(ms / 1000);
        tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
        lwip_setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    IPAddress remoteIP() const
    {
        struct sockaddr_in a;
        socklen_t len = sizeof a;
        if (_fd < 0 || lwip_getpeername(_fd, (struct sockaddr *)&a, &len) != 0) {
            return IPAddress();
        }
        return IPAddress(a.sin_addr.s_addr);
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

        /* Backlog 4, not 1: the accept poll runs from the main loop, which can be parked in a
         * panel refresh for tens of seconds, and a backlog of 1 refuses every connection
         * attempt that arrives in that window. Arduino's WiFiServer defaults to 4 as well. */
        if (lwip_bind(_listen, (struct sockaddr *)&a, sizeof a) != 0 ||
            lwip_listen(_listen, 4) != 0) {
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

    /* accept() is the current Arduino spelling; available() is the legacy one. Same call. */
    WiFiClient accept() { return acceptOne(); }
    WiFiClient available() { return acceptOne(); }

private:
    WiFiClient acceptOne()
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

public:
    uint16_t port() const { return _port; }

private:
    uint16_t _port = 0;
    int _listen = -1;
    bool _noDelay = false;
};

/* WiFiClass is GONE (phase C step 9b-ii). src/wifi_service.cpp drives esp_wifi/esp_netif
 * directly now, and main.cpp asks it for the link state through wifiLinkIsUp().
 *
 * It was removed rather than left as dead code because five of its methods were SILENT
 * NO-OPS -- setScanMethod, setSortMethod, setTxPower, setAutoReconnect and onEvent -- plus a
 * BSSID() that returned a static zero array. Leaving that where a future caller could find it
 * is worse than the shim's usual cost: a no-op that compiles is indistinguishable from a
 * feature that works. See the comment block on the replacement in wifi_service.cpp for what
 * each one broke.
 *
 * What remains in this file is WiFiServer/WiFiClient (the LAN data path, phase C step 9b-iii)
 * and the IPAddress they return. It goes with them.
 */

