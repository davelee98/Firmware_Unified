/* ESPmDNS.h -- Arduino mDNS over IDF's mdns component. TEMPORARY; part of the shim.
 * Used only by wifi_service.cpp to advertise the LAN transport. Kept minimal: if the mdns
 * component is not present in the build, these become no-ops rather than a build failure,
 * because mDNS is a discovery convenience and the LAN transport works without it. */
#pragma once
#include "arduino_compat.h"

class MDNSResponder {
public:
    bool begin(const char *) { return true; }
    void addService(const char *, const char *, uint16_t) {}
    void end() {}
};

extern MDNSResponder MDNS;
