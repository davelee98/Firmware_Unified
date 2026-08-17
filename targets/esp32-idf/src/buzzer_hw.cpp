#include "buzzer_hw.h"


// ---------------------------------------------------------------------------
// ESP32 -- IDF's LEDC driver, directly.
//
// This replaced a two-branch Arduino block selected by ESP_ARDUINO_VERSION_MAJOR: a 3.x
// pin-based path (ledcAttach/ledcWrite/ledcDetach) and a 2.x channel-based one
// (ledcSetup/ledcAttachPin/ledcWrite/ledcDetachPin). Under IDF that macro is defined nowhere,
// so `#if ESP_ARDUINO_VERSION_MAJOR >= 3` was `0 >= 3` and this target had SILENTLY been
// compiling the 2.x branch -- the one the reference firmware, on Arduino core 3.x, does not
// take. compat/ledc_compat.h implements only the 2.x calls, which is the corroborating
// evidence: the 3.x branch could not have linked.
//
// The fork is gone rather than resolved in favour of one side: there is one LEDC peripheral
// and one way to drive it from IDF, and a version macro from a framework this target no longer
// uses cannot select anything meaningful.
//
// Frequency now ROUNDS to the nearest Hz, which is the 3.x behaviour and therefore what ships.
// The 2.x branch computed a double and handed it to a uint32_t shim parameter, so it truncated
// -- up to 1 Hz flat, worst at the bottom of the range. Small, but it was a divergence the
// shim introduced rather than one anybody chose.
//
// Timer and channel selection is deliberately UNCHANGED, including its oddities: channel 7,
// timer `7 % LEDC_TIMER_MAX`, channel `7 % LEDC_CHANNEL_MAX`. Those modulos land differently
// per chip (8 channels on ESP32/S3 -> channel 7; 6 on C3/C6 -> channel 1) and that is exactly
// what is flashed today. Nothing else in the target uses LEDC, so there is no conflict to
// resolve and no reason to renumber inside a commit about removing Arduino.
// ---------------------------------------------------------------------------

#include "driver/ledc.h"
#include "od_hal_gpio.h"

static const uint8_t  kBuzzerLedcResBits = 10;         // 10-bit -> duty 0..1023
static const uint32_t kBuzzerLedcMaxDuty = 1023u;
static const uint8_t  kBuzzerLedcChannelId = 7;        // reserved buzzer channel

#define OD_BUZZER_LEDC_TIMER   ((ledc_timer_t)(kBuzzerLedcChannelId % LEDC_TIMER_MAX))
#define OD_BUZZER_LEDC_CHANNEL ((ledc_channel_t)(kBuzzerLedcChannelId % LEDC_CHANNEL_MAX))

// Pin currently attached to LEDC (0xFF = none). tone_stop is called on every rest step and at
// melody end even when nothing was attached, so this keeps it idempotent.
static uint8_t s_ledc_pin = 0xFF;

bool buzzer_hw_tone_start(uint8_t pin, uint32_t centihz, uint8_t duty_percent) {
    if (centihz == 0) {
        return false;
    }
    if (duty_percent == 0 || duty_percent > 100) {
        duty_percent = 50;
    }
    uint32_t freq_hz = (centihz + 50u) / 100u;         // round to integer Hz
    if (freq_hz == 0) {
        return false;
    }

    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = (ledc_timer_bit_t)kBuzzerLedcResBits;
    t.timer_num       = OD_BUZZER_LEDC_TIMER;
    t.freq_hz         = freq_hz;
    t.clk_cfg         = LEDC_AUTO_CLK;
    if (ledc_timer_config(&t) != ESP_OK) {
        return false;      // the Arduino 3.x path returned false on attach failure too
    }

    ledc_channel_config_t c = {};
    c.gpio_num   = pin;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = OD_BUZZER_LEDC_CHANNEL;
    c.timer_sel  = OD_BUZZER_LEDC_TIMER;
    c.duty       = 0;
    c.hpoint     = 0;
    if (ledc_channel_config(&c) != ESP_OK) {
        return false;
    }
    s_ledc_pin = pin;

    uint32_t duty = (kBuzzerLedcMaxDuty * (uint32_t)duty_percent) / 100u;
    if (duty == 0) {
        duty = 1;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, OD_BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, OD_BUZZER_LEDC_CHANNEL);
    return true;
}

void buzzer_hw_tone_stop(uint8_t pin) {
    if (s_ledc_pin == pin) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, OD_BUZZER_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, OD_BUZZER_LEDC_CHANNEL);
        // ledc_stop parks the output at level 0 and releases the channel's hold on the pad.
        // The 2.x path reached the same end state via gpio_reset_pin() inside the shim's
        // ledcDetachPin(); this is the driver's own way of saying it, and it does not disturb
        // the pad's other settings before the explicit drive-low below.
        ledc_stop(LEDC_LOW_SPEED_MODE, OD_BUZZER_LEDC_CHANNEL, 0);
        s_ledc_pin = 0xFF;
    }
    // Always leave the pin defined and LOW.
    od_hal_gpio_config_output(pin, false);
}

// ---------------------------------------------------------------------------
