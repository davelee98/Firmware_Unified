/* od_hal_adc for ESP-IDF. See od_hal_adc.h.
 *
 * Moved from compat/arduino_compat.cpp's analogRead/analogSetPinAttenuation/
 * analogReadResolution, unchanged in behaviour -- including the ADC1-only restriction, the
 * per-channel attenuation table, the lazy channel configuration, the warn-once bitmap and the
 * resolution shift. The shim's three functions now forward here, so there is exactly one owner
 * of the ADC1 unit handle: adc_oneshot_new_unit() on a unit that is already open fails, and
 * two owners would mean whichever driver initialised second silently read nothing.
 */

#include "od_hal_adc.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *ADC_TAG = "od_adc";

static adc_oneshot_unit_handle_t s_adc1 = NULL;
static int  s_adc_bits = 12;                        /* the SoC's native width */
static uint8_t s_atten[SOC_ADC_CHANNEL_NUM(0)];     /* per-channel, indexed by channel */
static bool s_atten_set[SOC_ADC_CHANNEL_NUM(0)];
static bool s_configured[SOC_ADC_CHANNEL_NUM(0)];

static bool adc_unit_ready(void)
{
    if (s_adc1) {
        return true;
    }
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&cfg, &s_adc1) != ESP_OK) {
        s_adc1 = NULL;
        return false;
    }
    return true;
}

/* Maps a GPIO to an ADC1 channel. False for pins that are not ADC1-capable. */
static bool adc_channel_for_pin(uint8_t pin, adc_channel_t *chan_out)
{
    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel((int)pin, &unit, &chan) != ESP_OK) {
        return false;
    }
    if (unit != ADC_UNIT_1) {
        return false;
    }
    if ((int)chan >= (int)SOC_ADC_CHANNEL_NUM(0)) {
        return false;
    }
    *chan_out = chan;
    return true;
}

bool od_hal_adc_pin_readable(uint8_t pin)
{
    adc_channel_t chan;
    return adc_channel_for_pin(pin, &chan);
}

void od_hal_adc_set_atten(uint8_t pin, od_hal_adc_atten_t atten)
{
    adc_channel_t chan;
    if (!adc_channel_for_pin(pin, &chan)) {
        return;
    }
    const adc_atten_t a = (atten == OD_ADC_ATTEN_0DB) ? ADC_ATTEN_DB_0 : ADC_ATTEN_DB_12;
    if (s_atten_set[chan] && s_atten[chan] == (uint8_t)a) {
        return;
    }
    s_atten[chan]      = (uint8_t)a;
    s_atten_set[chan]  = true;
    s_configured[chan] = false;   /* force a re-config on the next read */
}

void od_hal_adc_set_resolution(uint8_t bits)
{
    if (bits >= 9 && bits <= 12) {
        s_adc_bits = (int)bits;
    }
}

int od_hal_adc_read(uint8_t pin)
{
    adc_channel_t chan;
    if (!adc_channel_for_pin(pin, &chan)) {
        /* Once per pin, not once per sample: the battery path averages ten samples per call
         * and every MSD refresh drives it, so per-sample warnings buried the rest of the log.
         *
         * An unprovisioned device lands here with pin 0 -- globalConfig is memset to zero, so
         * battery_sense_pin reads 0 rather than the 0xFF "unset" sentinel the caller checks,
         * and GPIO0 is not an ADC1 input on any variant here. Worth saying exactly once. */
        static uint64_t s_warned = 0;   /* bitmap, one bit per GPIO */
        if (pin < 64 && !(s_warned & (1ULL << pin))) {
            s_warned |= (1ULL << pin);
            ESP_LOGW(ADC_TAG, "GPIO %u is not an ADC1 input; read returns 0 "
                              "(pin 0 usually means no battery_sense_pin is configured)",
                     (unsigned)pin);
        }
        return 0;
    }
    if (!adc_unit_ready()) {
        return 0;
    }
    if (!s_configured[chan]) {
        adc_oneshot_chan_cfg_t ccfg = {
            .atten    = s_atten_set[chan] ? (adc_atten_t)s_atten[chan] : ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_config_channel(s_adc1, chan, &ccfg) != ESP_OK) {
            return 0;
        }
        s_configured[chan] = true;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc1, chan, &raw) != ESP_OK) {
        return 0;
    }
    /* The driver yields the SoC's native width; a lower requested resolution is a shift.
     * Shifting rather than rescaling keeps 12 -> 12 exact. */
    if (s_adc_bits < 12) {
        raw >>= (12 - s_adc_bits);
    }
    return raw;
}
