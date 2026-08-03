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
#include "esp_log.h"

#include "esp_adc/adc_oneshot.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

SerialCompat Serial;

TwoWire  Wire;
SPIClass SPI;
EspClass ESP;
MDNSResponder MDNS;

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
 * Arduino's analogRead over IDF's oneshot driver. Replaces a `return 0` stub whose two
 * callers both turned it into a plausible-looking wrong answer rather than an obvious
 * failure -- 0.0 V battery, and a permanently-pressed ADC ladder button. See the note in
 * arduino_compat.h.
 *
 * Real state (one unit handle, one attenuation table) so this cannot be static inline.
 */
extern "C" {

static const char *ADC_TAG = "od_adc";

/* Only ADC1 is used: ADC2 shares its hardware with the WiFi radio on ESP32/S2/S3 and reads
 * fail with ESP_ERR_TIMEOUT whenever WiFi is up -- and this target's boards run WiFi. A pin
 * that maps to ADC2 is reported as unreadable rather than read unreliably. */
static adc_oneshot_unit_handle_t s_adc1 = nullptr;
static int  s_adc_bits = 12;                 /* Arduino's ESP32 default */
static uint8_t s_atten[SOC_ADC_CHANNEL_NUM(0)];   /* per-channel, indexed by channel */
static bool s_atten_set[SOC_ADC_CHANNEL_NUM(0)];
static bool s_configured[SOC_ADC_CHANNEL_NUM(0)];

static bool adc_unit_ready(void)
{
    if (s_adc1) {
        return true;
    }
    adc_oneshot_unit_init_cfg_t cfg = {};
    cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&cfg, &s_adc1) != ESP_OK) {
        s_adc1 = nullptr;
        return false;
    }
    return true;
}

/* Maps a GPIO to an ADC1 channel. Returns false for pins that are not ADC1-capable. */
static bool adc_channel_for_pin(int pin, adc_channel_t *chan_out)
{
    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel(pin, &unit, &chan) != ESP_OK) {
        return false;
    }
    if (unit != ADC_UNIT_1) {
        return false;
    }
    *chan_out = chan;
    return true;
}

void analogSetPinAttenuation(int pin, int atten)
{
    adc_channel_t chan;
    if (!adc_channel_for_pin(pin, &chan) || (int)chan >= (int)SOC_ADC_CHANNEL_NUM(0)) {
        return;
    }
    /* Arduino's ADC_0db/ADC_11db constants. ADC_ATTEN_DB_11 is deprecated in IDF 5.x in
     * favour of DB_12, which is the same setting under a name that matches the silicon. */
    adc_atten_t a = (atten == ADC_0db) ? ADC_ATTEN_DB_0 : ADC_ATTEN_DB_12;
    if (s_atten_set[chan] && s_atten[chan] == (uint8_t)a) {
        return;
    }
    s_atten[chan]     = (uint8_t)a;
    s_atten_set[chan] = true;
    s_configured[chan] = false;   /* force a re-config on the next read */
}

void analogReadResolution(int bits)
{
    if (bits >= 9 && bits <= 12) {
        s_adc_bits = bits;
    }
}

int analogRead(int pin)
{
    adc_channel_t chan;
    if (!adc_channel_for_pin(pin, &chan) || (int)chan >= (int)SOC_ADC_CHANNEL_NUM(0)) {
        /* Once per pin, not once per sample. readBatteryVoltageUncached() averages ten
         * samples per call and updatemsdata() drives it, so an unconfigured pin produced ten
         * identical warnings every refresh -- enough to bury the rest of the log.
         *
         * An unprovisioned device lands here with pin 0: globalConfig is memset to zero, so
         * battery_sense_pin reads 0 rather than the 0xFF "unset" sentinel the caller checks,
         * and GPIO0 is not an ADC1 input on any variant here. That is worth saying once. */
        static uint64_t s_warned = 0;   /* bitmap, one bit per GPIO */
        if (pin >= 0 && pin < 64) {
            if (!(s_warned & (1ULL << pin))) {
                s_warned |= (1ULL << pin);
                ESP_LOGW(ADC_TAG, "GPIO %d is not an ADC1 input; analogRead returns 0 "
                                  "(pin 0 usually means no battery_sense_pin is configured)", pin);
            }
        }
        return 0;
    }
    if (!adc_unit_ready()) {
        return 0;
    }
    if (!s_configured[chan]) {
        adc_oneshot_chan_cfg_t ccfg = {};
        ccfg.atten    = s_atten_set[chan] ? (adc_atten_t)s_atten[chan] : ADC_ATTEN_DB_12;
        ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(s_adc1, chan, &ccfg) != ESP_OK) {
            return 0;
        }
        s_configured[chan] = true;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc1, chan, &raw) != ESP_OK) {
        return 0;
    }
    /* The driver yields the SoC's native width (12 bits on every variant here); Arduino's
     * analogReadResolution() rescales. Shifting rather than multiplying keeps 12 -> 12 exact. */
    if (s_adc_bits < 12) {
        raw >>= (12 - s_adc_bits);
    }
    return raw;
}

/* ------------------------------------------------------------------ die temperature */

float temperatureRead(void)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    static temperature_sensor_handle_t s_tsens = nullptr;
    if (!s_tsens) {
        /* -10..80 C covers the panel's rated operating range with margin; the driver picks
         * the matching internal range setting. */
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
            s_tsens = nullptr;
            return -999.0f;
        }
        if (temperature_sensor_enable(s_tsens) != ESP_OK) {
            temperature_sensor_uninstall(s_tsens);
            s_tsens = nullptr;
            return -999.0f;
        }
    }
    float c = 0.0f;
    if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return -999.0f;
    }
    return c;
#else
    /* Classic ESP32 has no usable die sensor. -999.0 is the sentinel the NRF path uses for
     * "no reading", and readChipTemperature()'s callers already understand it. */
    return -999.0f;
#endif
}

} /* extern "C" */
