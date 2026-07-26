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
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#ifdef __cplusplus
#include <string>
#endif

/* The imported sources gate platform code on Arduino's arch macro. Defining it here keeps
 * that gating working during phase B without editing those files; it disappears with the
 * shim, at which point those #ifdefs must be rewritten against IDF's own target macros. */
#ifndef ARDUINO_ARCH_ESP32
#define ARDUINO_ARCH_ESP32 1
#endif

/* ---------------------------------------------------------------- pin levels + modes */

#define HIGH 1
#define LOW  0

#define INPUT         0x01
#define OUTPUT        0x02
#define INPUT_PULLUP  0x05
#define INPUT_PULLDOWN 0x09

/* Radix constants for String(v, radix) and Serial.print(v, radix). */
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

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

static inline void delay(long ms)
{
    /* vTaskDelay rounds DOWN to whole ticks; at the default 100 Hz tick a delay(5) would
     * become 0 and busy-spin the caller's logic. Round up so a short delay is never a no-op. */
    if (ms < 0) ms = 0;
    TickType_t ticks = (TickType_t)(((uint32_t)ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
    vTaskDelay(ticks ? ticks : 1);
}

static inline void delayMicroseconds(long us)
{
    esp_rom_delay_us((uint32_t)(us < 0 ? 0 : us));
}

static inline void yield(void)
{
    taskYIELD();
}

/* ---------------------------------------------------------------- interrupts
 * Arduino's GPIO interrupt API over IDF's gpio ISR service. Used by touch_input and the
 * wake button. The destination is od_hal_gpio's config_irq (docs/SHARED_API_DESIGN.md),
 * whose contract is stricter than Arduino's and worth remembering while reading these call
 * sites: an ISR handler may SET A FLAG ONLY.
 */
#define RISING   GPIO_INTR_POSEDGE
#define FALLING  GPIO_INTR_NEGEDGE
#define CHANGE   GPIO_INTR_ANYEDGE

typedef void (*od_isr_fn)(void);

/* Arduino maps a pin to an "interrupt number"; on ESP32 they are the same thing. */
static inline int digitalPinToInterrupt(int pin) { return pin; }

static inline void attachInterrupt(int pin, od_isr_fn fn, int mode)
{
    /* Idempotent: the ISR service is installed once, and re-attaching a pin replaces its
     * handler rather than erroring, which is what the Arduino callers assume. */
    static bool isr_service_started = false;
    if (!isr_service_started) {
        gpio_install_isr_service(0);
        isr_service_started = true;
    }
    gpio_set_intr_type((gpio_num_t)pin, (gpio_int_type_t)mode);
    gpio_isr_handler_remove((gpio_num_t)pin);
    gpio_isr_handler_add((gpio_num_t)pin, (gpio_isr_t)fn, NULL);
    gpio_intr_enable((gpio_num_t)pin);
}

static inline void detachInterrupt(int pin)
{
    gpio_intr_disable((gpio_num_t)pin);
    gpio_isr_handler_remove((gpio_num_t)pin);
}

/* Arduino's global interrupt enable/disable. portDISABLE_INTERRUPTS is per-core on ESP32,
 * which is NOT the same guarantee -- these call sites need auditing when the code they
 * guard moves to shared/core, which has no global-disable primitive at all and needs an
 * explicit od_hal irq-lock (flagged in DESIGN_REVIEW § "Big-picture soundness"). */
/* Arduino ADC. analogRead returns 12-bit on ESP32 by default; oneshot ADC is the IDF
 * replacement and these call sites move to it when device_control is touched. */
#define ADC_11db 3
#define ADC_0db  0
static inline void analogSetPinAttenuation(int, int) {}
static inline int  analogRead(int) { return 0; }
static inline void analogReadResolution(int) {}

/* attachInterruptArg passes a context pointer; IDF's handler takes void* natively. */
static inline void attachInterruptArg(int pin, void (*fn)(void *), void *arg, int mode)
{
    static bool started = false;
    if (!started) { gpio_install_isr_service(0); started = true; }
    gpio_set_intr_type((gpio_num_t)pin, (gpio_int_type_t)mode);
    gpio_isr_handler_remove((gpio_num_t)pin);
    gpio_isr_handler_add((gpio_num_t)pin, fn, arg);
    gpio_intr_enable((gpio_num_t)pin);
}

/* On ESP32 the Arduino pin number IS the GPIO number. */
static inline int digitalPinToGPIONumber(int pin) { return pin; }

/* Internal die-temperature sensor. Arduino exposes it as temperatureRead(); IDF has a
 * driver (esp_driver_tsens) that needs explicit setup, so this returns a sentinel rather
 * than a plausible-looking lie. The advert clamps temperature anyway, and a wrong reading
 * there would be published to every scanning host. */
static inline float temperatureRead(void) { return 0.0f; }

static inline void noInterrupts(void) { portDISABLE_INTERRUPTS(); }
static inline void interrupts(void)   { portENABLE_INTERRUPTS(); }

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
    /* String(v, HEX) -- the two-arg radix form. Only HEX and DEC appear in the sources. */
    String(unsigned long v, int radix)
    { char b[24]; snprintf(b, sizeof b, radix == 16 ? "%lX" : "%lu", v); s_ = b; }
    String(unsigned v, int radix)
    { char b[16]; snprintf(b, sizeof b, radix == 16 ? "%X" : "%u", v); s_ = b; }

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

    char charAt(size_t i) const { return (*this)[i]; }
    void setCharAt(size_t i, char c) { if (i < s_.size()) s_[i] = c; }
    String substring(size_t from) const
    { return from >= s_.size() ? String() : String(s_.substr(from)); }
    String substring(size_t from, size_t to) const
    { if (from >= s_.size() || to <= from) return String(); return String(s_.substr(from, to - from)); }
    int indexOf(char c) const { size_t p = s_.find(c); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const char *n) const
    { if (!n) return -1; size_t p = s_.find(n); return p == std::string::npos ? -1 : (int)p; }
    bool startsWith(const char *p) const { return p && s_.rfind(p, 0) == 0; }
    void toUpperCase() { for (auto &ch : s_) ch = (char)toupper((unsigned char)ch); }
    void toLowerCase() { for (auto &ch : s_) ch = (char)tolower((unsigned char)ch); }
    void trim()
    { size_t b = s_.find_first_not_of(" \t\r\n"); size_t e = s_.find_last_not_of(" \t\r\n");
      s_ = (b == std::string::npos) ? "" : s_.substr(b, e - b + 1); }

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

/* ESP.* -- three accessors, mapped onto their IDF equivalents. */
class EspClass {
public:
    uint32_t getFreeHeap() const;
    uint32_t getMinFreeHeap() const;
    uint64_t getEfuseMac() const;
};

extern EspClass ESP;

#include "ledc_compat.h"   /* Arduino LEDC PWM, used by buzzer_hw */

#endif /* __cplusplus */
