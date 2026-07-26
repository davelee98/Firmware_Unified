/* arduino_compat.h -- TEMPORARY. Maps the Arduino surface onto ESP-IDF.
 *
 * Phase B of the ESP32 import. Its only job is to get this target to LINK AND BOOT ON
 * HARDWARE EARLY, so that every later step -- each subsystem promoted into shared/core -- is
 * bisectable against a known-good baseline (docs/MIGRATION.md § "The ESP32 import is
 * different").
 *
 * THIS IS NOT A PORTABILITY LAYER. Nothing new may be written against it. Its usage is
 * counted by compat/ratchet.sh and may only decrease; when it reaches zero this file and the
 * ratchet are deleted together. If it is still here when the last subsystem lands, the port
 * is not done.
 *
 * Scope note: this covers the mechanical Arduino APIs -- GPIO, time, Serial, String. Two
 * things deliberately are NOT shimmed because they are genuine rewrites, not translations:
 * the NimBLE C-API port (ble_init.cpp, esp32_ble_callbacks.h) and the panel backends
 * (bb_epaper / FastEPD / Seeed_GFX). See README.md § "What phase B still needs".
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#ifdef __cplusplus
#include <string>
#endif

/* ---------------------------------------------------------------- pin levels + modes */

#define HIGH 1
#define LOW  0

#define INPUT         0x01
#define OUTPUT        0x02
#define INPUT_PULLUP  0x05
#define INPUT_PULLDOWN 0x09

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- GPIO
 * Arduino's pinMode/digitalWrite take a plain int pin; IDF wants a gpio_num_t and a config
 * struct. These are the 102 pinMode / 87 digitalWrite call sites in the census
 * (docs/TOOLCHAINS.md § "Arduino API census"). Note that the real destination for these is
 * od_hal_gpio, NOT this file -- shared/core must never see a raw pin number.
 */
static inline void pinMode(int pin, int mode)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)pin),
        .mode         = (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en   = (mode == INPUT_PULLUP)   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
        .pull_down_en = (mode == INPUT_PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static inline void digitalWrite(int pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

static inline int digitalRead(int pin)
{
    return gpio_get_level((gpio_num_t)pin);
}

/* ---------------------------------------------------------------- time
 * millis() wraps at 2^32 ms just as Arduino's does. esp_timer_get_time() is int64 microseconds
 * since boot, so the truncation is deliberate and matches the semantics the callers were
 * written against -- they compare with subtraction, which is wrap-safe.
 */
static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline uint32_t micros(void)
{
    return (uint32_t)esp_timer_get_time();
}

static inline void delay(uint32_t ms)
{
    /* vTaskDelay rounds DOWN to whole ticks; at the default 100 Hz tick a delay(5) would
     * become 0 and busy-spin the caller's logic. Round up so a short delay is never a no-op. */
    TickType_t ticks = (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
    vTaskDelay(ticks ? ticks : 1);
}

static inline void delayMicroseconds(uint32_t us)
{
    esp_rom_delay_us(us);
}

static inline void yield(void)
{
    taskYIELD();
}

#ifdef __cplusplus
}   /* extern "C" */
#endif

/* ---------------------------------------------------------------- String
 *
 * The single largest item in the census: 575 call sites. This is a minimal std::string
 * wrapper providing only what the imported sources use.
 *
 * It exists to be deleted. shared/ is plain C and CI rejects the token `String` there
 * outright, so every String on a path destined for shared/core has to go regardless -- which
 * is the argument docs/TOOLCHAINS.md makes for why the Arduino removal and the shared/
 * extraction are substantially the same work.
 */
#ifdef __cplusplus

class String {
public:
    String() {}
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    explicit String(int v)      { char b[16]; snprintf(b, sizeof b, "%d", v);   s_ = b; }
    explicit String(unsigned v) { char b[16]; snprintf(b, sizeof b, "%u", v);   s_ = b; }
    explicit String(long v)     { char b[24]; snprintf(b, sizeof b, "%ld", v);  s_ = b; }
    explicit String(char c)     { s_ = std::string(1, c); }

    const char *c_str() const { return s_.c_str(); }
    size_t length() const     { return s_.size(); }
    bool isEmpty() const      { return s_.empty(); }

    String &operator+=(const String &o) { s_ += o.s_; return *this; }
    String &operator+=(const char *o)   { if (o) s_ += o; return *this; }
    String &operator+=(char c)          { s_ += c; return *this; }

    friend String operator+(String a, const String &b) { a += b; return a; }
    friend String operator+(String a, const char *b)   { a += b; return a; }

    bool operator==(const String &o) const { return s_ == o.s_; }
    bool operator==(const char *o) const   { return o && s_ == o; }
    bool operator!=(const String &o) const { return !(*this == o); }

    char operator[](size_t i) const { return i < s_.size() ? s_[i] : '\0'; }

private:
    std::string s_;
};

/* ---------------------------------------------------------------- Serial
 *
 * The source funnels logging through writeSerial(String, bool) already (docs/
 * SHARED_API_DESIGN.md § od_hal_log notes this is "the same shape, wrong signature/type"),
 * so this only has to satisfy the direct Serial.* uses that remain.
 *
 * Everything here is bound by the no-secrets rule (docs/ARCHITECTURE.md § "Secrets are never
 * logged verbatim"): presence and length, never content, at every level including debug.
 */
/* Stream: od_log.cpp keeps a `Stream *` and writes lines to it. Only the write path is used,
 * so this is the abstract sink and nothing more -- the destination for this is od_hal_log,
 * a single `void od_hal_log(const char *line)` (docs/SHARED_API_DESIGN.md). */
class Stream {
public:
    virtual ~Stream() {}
    virtual size_t write(const uint8_t *b, size_t n) { return fwrite(b, 1, n, stdout); }
    size_t print(const char *s)   { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t println(const char *s) { size_t n = print(s); n += print("\n"); return n; }
    size_t println(const String &s) { return println(s.c_str()); }
    void flush() { fflush(stdout); }
};

class SerialCompat : public Stream {
public:
    void begin(unsigned long) {}
    size_t write(const uint8_t *b, size_t n) override { return fwrite(b, 1, n, stdout); }
    void end() {}
    operator bool() const { return true; }
    void flush() { fflush(stdout); }

    size_t print(const char *s)   { return s ? fputs(s, stdout), strlen(s) : 0; }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t print(int v)           { return printf("%d", v); }
    size_t print(unsigned v)      { return printf("%u", v); }

    size_t println()                { return printf("\n"); }
    size_t println(const char *s)   { return printf("%s\n", s ? s : ""); }
    size_t println(const String &s) { return println(s.c_str()); }
    size_t println(int v)           { return printf("%d\n", v); }

    size_t printf_(const char *f, ...) { (void)f; return 0; }
};

extern SerialCompat Serial;

#endif /* __cplusplus */
