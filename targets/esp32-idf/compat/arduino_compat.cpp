/* Definitions for the shim's one global. Header-only would need C++17 inline variables and
 * the source builds as C++11/14 under Arduino today; keeping a .cpp avoids depending on the
 * standard level while the port is in flux. Dies with the shim. */
#include "arduino_compat.h"
#include "Wire.h"

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"

#include "od_hal_adc.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

SerialCompat Serial;

TwoWire  Wire;
/* SPIClass SPI is GONE from here -- it moved to vendor/fastepd/fastepd_adapter.cpp
 * (2026-08-04). It is the FastEPD vendor adapter's global, not the shim's, and while its
 * definition lived in this file the adapter could not be taken off the component's global
 * include path. compat/ dies; the adapter does not. */
EspClass ESP;

/* INTERNAL DRAM ONLY -- MALLOC_CAP_INTERNAL, matching Arduino-ESP32's EspClass exactly:
 *
 *     uint32_t EspClass::getFreeHeap()    { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
 *     uint32_t EspClass::getMinFreeHeap() { return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL); }
 *
 * These were esp_get_free_heap_size() / esp_get_minimum_free_heap_size(), i.e. MALLOC_CAP_DEFAULT.
 * With CONFIG_SPIRAM_USE_MALLOC=y that INCLUDES PSRAM, so on an 8 MB-PSRAM S3 the log read
 *
 *     Heap: free=8437776 min=8437724
 *
 * -- 8.4 MB, with the minimum barely below the current free, because 8 MB of PSRAM swamps a
 * DRAM figure that upstream measured bottoming out at 48-264 BYTES on a wake cycle running a
 * BLE pipe-write.
 *
 * That is not a cosmetic difference. Internal-DRAM exhaustion is the exact failure
 * Firmware's dc60c8a ("reclaim internal DRAM -- PSRAM LAN RX buffer, PSRAM-only WiFi/tinfl")
 * was written to fix, and its own open item is "not yet verified on hardware: the min-heap
 * figure on a wake cycle with a BLE pipe-write (expect ~16 KB where it was 48-264 B)". With
 * PSRAM folded in, that number is unreadable and the fix unverifiable -- the heap log would
 * have looked healthy through the six PANIC resets that started the investigation.
 *
 * ESP.getFreeHeap() is NOT a general "how much memory is left" question on a PSRAM part. It is
 * specifically the DRAM-pressure question, which is why Arduino scoped it this way and why
 * getFreePsram() is a separate accessor. Do not "improve" these to MALLOC_CAP_DEFAULT. */
uint32_t EspClass::getFreeHeap() const    { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
uint32_t EspClass::getMinFreeHeap() const { return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL); }

/* PSRAM free, reported separately exactly as Arduino does. Added because the heap log is now
 * internal-only and a reader still needs to see that PSRAM exists and is being used -- the LAN
 * RX buffer relocation in dc60c8a depends on it. Returns 0 with no PSRAM, which is also what
 * Arduino's psramFound() guard produces. */
uint32_t EspClass::getFreePsram() const   { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

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

/* External linkage because two vendored libraries need it -- see the note in
 * arduino_compat.h. The truncation to 32 bits is deliberate: callers compare with
 * subtraction, which is wrap-safe, and Arduino's millis() wraps the same way. */
uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ ADC
 *
 * The implementation MOVED to hal/od_hal_adc.{h,c} in phase C step 7, unchanged; these three
 * are forwarders. They remain because display_service.cpp still calls analogRead() -- it comes
 * off the shim at step 10 -- and IDF's oneshot driver has ONE ADC1 unit handle. A second
 * adc_oneshot_new_unit() on an already-open unit fails, so two owners would mean whichever
 * driver initialised second silently read nothing, which for these callers is a plausible
 * 0.0 V battery and a permanently-pressed ladder button rather than an obvious failure.
 */
extern "C" {

void analogSetPinAttenuation(int pin, int atten)
{
    if (pin < 0) {
        return;
    }
    od_hal_adc_set_atten((uint8_t)pin,
                         (atten == ADC_0db) ? OD_ADC_ATTEN_0DB : OD_ADC_ATTEN_12DB);
}

void analogReadResolution(int bits)
{
    if (bits >= 0) {
        od_hal_adc_set_resolution((uint8_t)bits);
    }
}

int analogRead(int pin)
{
    if (pin < 0) {
        return 0;
    }
    return od_hal_adc_read((uint8_t)pin);
}

/* ------------------------------------------------------------------ die temperature */

float temperatureRead(void)
{
    /* Forwarder. The implementation MOVED to hal/od_hal_adc.c (phase C step 14), unchanged,
     * for the same reason analogRead() did in step 7: IDF installs ONE temperature_sensor
     * handle, so the HAL must own it or two installers silently fight. This remains only
     * because display_service.cpp's TARGET_NRF arm still compiles against the shim. */
    return od_hal_adc_die_temp_c();
}

} /* extern "C" */
