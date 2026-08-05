#include "buzzer_hw.h"

// ---------------------------------------------------------------------------
// nRF52 (Adafruit core) -- HardwarePWM instance HwPWM3.
//   HwPWM0..2 are left for core/other features; HwPWM3 exists on nRF52840.
//   16 MHz base clock, 15-bit COUNTERTOP. We walk the prescaler DIV_1..DIV_128
//   and pick the smallest divider whose top = round(clk*100 / centihz) fits in
//   15 bits (<= 32767), giving <= ~0.5 cent pitch error across the range.
//   Ownership is taken/released cooperatively so the peripheral is only held
//   while a tone is actually sounding. Runs alongside the SoftDevice.
// ---------------------------------------------------------------------------
#if defined(ARDUINO_ARCH_NRF52)

#include <Arduino.h>          // pinMode/digitalWrite; leaves with Bluefruit at migration step 4
#include "HardwarePWM.h"

// Non-zero cooperative ownership token ("BZZ!").
static const uint32_t kBuzzerPwmToken = 0x425A5A21u;
static const uint32_t kBuzzerPwmClockHz = 16000000u;   // DIV_1 base clock
static const uint32_t kBuzzerPwmMaxTop = 32767u;       // 15-bit COUNTERTOP

bool buzzer_hw_tone_start(uint8_t pin, uint32_t centihz, uint8_t duty_percent) {
    if (centihz == 0) {
        return false;
    }
    if (duty_percent == 0 || duty_percent > 100) {
        duty_percent = 50;
    }

    // Prescaler walk: DIV_1..DIV_128 map to clock shifts of 0..7.
    uint8_t div_exp = 0;
    uint32_t top = 0;
    for (div_exp = 0; div_exp <= 7; div_exp++) {
        uint32_t clk = kBuzzerPwmClockHz >> div_exp;
        uint64_t t = ((uint64_t)clk * 100u + (centihz / 2u)) / centihz;   // round
        if (t == 0) {
            t = 1;
        }
        if (t <= kBuzzerPwmMaxTop) {
            top = (uint32_t)t;
            break;
        }
    }
    if (top == 0) {
        return false;   // frequency too low even at DIV_128 (should not happen in-range)
    }

    // Ownership: take it only if we don't already hold it.
    if (!HwPWM3.isOwner(kBuzzerPwmToken)) {
        if (!HwPWM3.takeOwnership(kBuzzerPwmToken)) {
            return false;   // owned by something else
        }
    }

    // PRESCALER/COUNTERTOP must be set while stopped; writePin -> begin() then
    // reloads them and restarts the sequence.
    if (HwPWM3.enabled()) {
        HwPWM3.stop();
    }
    HwPWM3.setClockDiv((uint8_t)div_exp);          // PWM_PRESCALER_PRESCALER_DIV_x == div_exp
    HwPWM3.setMaxValue((uint16_t)top);
    HwPWM3.addPin(pin);

    uint32_t val = (top * (uint32_t)duty_percent) / 100u;
    if (val == 0) {
        val = 1;
    }
    if (val >= top) {
        val = top - 1;
    }
    HwPWM3.writePin(pin, (uint16_t)val);
    return true;
}

void buzzer_hw_tone_stop(uint8_t pin) {
    if (HwPWM3.isOwner(kBuzzerPwmToken)) {
        if (HwPWM3.checkPin(pin)) {
            HwPWM3.removePin(pin);
        }
        HwPWM3.stop();
        HwPWM3.releaseOwnership(kBuzzerPwmToken);
    }
    // Always leave the pin defined and LOW.
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

#elif defined(TARGET_ESP32)

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
#else
#error "buzzer_hw: unsupported platform (need ARDUINO_ARCH_NRF52 or TARGET_ESP32)"
#endif
