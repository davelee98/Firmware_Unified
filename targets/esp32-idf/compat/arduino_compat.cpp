/* Definitions for the shim's one global. Header-only would need C++17 inline variables and
 * the source builds as C++11/14 under Arduino today; keeping a .cpp avoids depending on the
 * standard level while the port is in flux. Dies with the shim. */
#include "arduino_compat.h"
#include "Wire.h"
#include "SPI.h"
#include "ESPmDNS.h"

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

SerialCompat Serial;

TwoWire  Wire;
SPIClass SPI;
EspClass ESP;
MDNSResponder MDNS;

uint32_t EspClass::getFreeHeap() const    { return (uint32_t)esp_get_free_heap_size(); }
uint32_t EspClass::getMinFreeHeap() const { return (uint32_t)esp_get_minimum_free_heap_size(); }

uint64_t EspClass::getEfuseMac() const
{
    uint8_t mac[6] = {0};
    /* Arduino returns the factory MAC as a 48-bit value in a uint64. esp_efuse_mac_get_default
     * fills big-endian bytes, so pack in the same order Arduino does or every derived device
     * id -- which the advert and the device name both use -- changes. */
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) {
        v |= ((uint64_t)mac[i]) << (8 * i);
    }
    return v;
}

/* External linkage, not static inline: bb_epaper's translation unit sees only the
 * declaration from bb_epaper.h and needs a real symbol to link against. */
void delay(long ms)
{
    if (ms < 0) ms = 0;
    TickType_t ticks = (TickType_t)(((uint32_t)ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
    vTaskDelay(ticks ? ticks : 1);
}

void delayMicroseconds(long us)
{
    esp_rom_delay_us((uint32_t)(us < 0 ? 0 : us));
}
