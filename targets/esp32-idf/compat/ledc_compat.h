/* Arduino LEDC (PWM) over IDF's ledc driver. TEMPORARY; part of the shim.
 * Arduino's 2.x API is channel-based; IDF wants an explicit timer + channel pair, so the
 * channel number is reused for both.
 *
 * The last APP-code user, buzzer_hw.cpp, left in phase C step 4 (2026-08-04) and now calls
 * driver/ledc.h directly. This file survives for third_party/FastEPD/src/FastEPD.inl alone
 * (its front-light control: ledcSetup/ledcAttachPin/ledcWrite, 11 call sites), so it is pinned
 * by a vendored library exactly as delay()/millis() are -- see compat/SHIM_BUDGET's endgame
 * note. It cannot be deleted by clearing app code, and it is not counted against the budget. */
#pragma once
#include "arduino_compat.h"
#include "driver/ledc.h"

static inline void ledcSetup(uint8_t ch, uint32_t freq, uint8_t resBits)
{
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = (ledc_timer_bit_t)resBits;
    t.timer_num       = (ledc_timer_t)(ch % LEDC_TIMER_MAX);
    t.freq_hz         = freq;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
}

static inline void ledcAttachPin(uint8_t pin, uint8_t ch)
{
    ledc_channel_config_t c = {};
    c.gpio_num   = pin;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = (ledc_channel_t)(ch % LEDC_CHANNEL_MAX);
    c.timer_sel  = (ledc_timer_t)(ch % LEDC_TIMER_MAX);
    c.duty       = 0;
    c.hpoint     = 0;
    ledc_channel_config(&c);
}

static inline void ledcWrite(uint8_t ch, uint32_t duty)
{
    ledc_channel_t c = (ledc_channel_t)(ch % LEDC_CHANNEL_MAX);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, c, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, c);
}

static inline void ledcDetachPin(uint8_t pin)
{
    gpio_reset_pin((gpio_num_t)pin);
}
