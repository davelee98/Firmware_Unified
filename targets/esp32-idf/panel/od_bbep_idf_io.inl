/* od_bbep_idf_io.inl -- OpenDisplay's bb_epaper IO backend for ESP-IDF.
 *
 * Replaces third_party/bb_epaper/esp_idf/esp_generic.inl. Full rationale, the measured
 * function inventory and the defect list it addresses are in docs/BBEPAPER_IO_BACKENDS.md;
 * this comment covers only what a reader of THIS file needs.
 *
 * WHY OURS RATHER THAN UPSTREAM'S. Every other OpenDisplay target already writes its own
 * backend -- Firmware_NRF54 has nrf54_zephyr_io.inl (206 lines, zero local patches) and
 * Firmware_Silabs has silabs_efr32_io.inl (315 lines, zero). ESP-IDF was the only target
 * borrowing upstream's, and it accumulated four OD-PATCHes plus a live bug. bb_epaper is
 * DESIGNED for this: it selects an IO backend and each backend is one small file. This is the
 * supported porting path, not a fork.
 *
 * WHAT IS AND IS NOT IN SCOPE. All panel knowledge -- init sequences, LUTs, chip quirks, BUSY
 * polarity -- stays in bb_ep.inl, untouched. In particular the RST pulses (bb_ep.inl:4041/4043
 * in bbepWakeUp, :4281/4283 in bbepSendCMDSequence) and the BUSY poll loop (:3995 bbepWaitBusy,
 * :4031 bbepIsBusy) are PANEL logic that calls digitalWrite/digitalRead directly, bypassing
 * this file's abstraction. This backend therefore cannot and does not change reset timing or
 * BUSY behaviour. It owns the SPI lifecycle, the byte path, and the two GPIO primitives.
 *
 * HOW IT IS SELECTED. Not by patching bb_epaper.cpp's #ifdef chain (what Firmware_NRF54 did).
 * od_bbep.cpp includes bb_epaper.h, then this file, then bb_ep.inl and bb_ep_gfx.inl, and the
 * build excludes bb_epaper.cpp entirely. That needs zero edits to vendored files, and it drops
 * bb_epaper.cpp's 775-line C++ BBEPAPER class -- which this project never references and which
 * was the only consumer of pinMode() and millis(). See panel/od_bbep.cpp.
 *
 * WHAT THIS FILE MUST DEFINE, measured rather than guessed (docs/BBEPAPER_IO_BACKENDS.md §2):
 *
 *   contractual   bbepInitIO, bbepWriteCmd, bbepWriteData, bbepCMD2, bbepSetCS2
 *   primitives    digitalWrite (4 calls in bb_ep.inl), digitalRead (3)
 *   private       od_bbep_spi_write -- the name is ours; upstream calls it spi_write and
 *                 arduino_io.inl calls it SPI_Write, so it is not part of the contract
 *   extension     bbepDeInitIO -- NOT upstream. See the teardown note below.
 *
 * WHAT IT DELIBERATELY DOES NOT DEFINE:
 *
 *   delay()             already has an external definition in compat/arduino_compat.cpp, which
 *                       bb_ep.inl's 10 calls have been binding to ever since upstream's copy
 *                       was #if 0'd out. Defining it here would be a duplicate symbol.
 *   pinMode(), millis() only bb_epaper.cpp's C++ class called these, and it is no longer
 *                       compiled. bb_epaper.h still declares them; an unused declaration is
 *                       fine.
 *   delayMicroseconds, delayCycles, mymemset, mymemcpy, i2str, i2strf
 *                       zero callers across every compiled bb_epaper source. Upstream carries
 *                       six dead functions; we do not.
 */

#ifndef OD_BBEP_IDF_IO_INL
#define OD_BBEP_IDF_IO_INL
/* NOT __ESP_IDF_IO__: esp_generic.inl and esp_main_io.inl already share that guard, so reusing
 * it would silently suppress one of them if the include order ever changed. */

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

/* Arduino spellings bb_ep.inl uses. Guarded: this file is never compiled together with
 * compat/arduino_compat.h today, but a future TU that includes both must not fail on a
 * redefinition, and the values are identical either way. */
#ifndef INPUT
#define INPUT 0
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 1
#endif
#ifndef OUTPUT
#define OUTPUT 2
#endif
#ifndef DISABLED
#define DISABLED 3
#endif
#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif

/* No PROGMEM on this architecture -- flash is memory-mapped, so a "far" read is a plain read.
 * bb_ep.inl uses these for its panel init tables. */
#ifndef pgm_read_byte
#define pgm_read_byte(a)  (*(const uint8_t *)(a))
#endif
#ifndef pgm_read_word
#define pgm_read_word(a)  (*(const uint16_t *)(a))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif

static const char *OD_BBEP_TAG = "od_bbep";

/* One constant for both the bus's max_transfer_sz and the chunk size in od_bbep_spi_write().
 * Upstream has three numbers that disagree (max_transfer_sz 4096, a 4000-byte chunk, and a
 * "full duplex mode" comment on a half-duplex device). A 1872-wide 1bpp row is 234 B and the
 * widest panel plane here is well under this, so the chunk loop rarely iterates. */
#define OD_BBEP_MAX_XFER 4096

/* Defined in bb_ep.inl, which is included AFTER this file, so they must be declared here.
 * Both are called from below: bbepWakeUp() from bbepWriteCmd() when the controller is asleep,
 * bbepSendCMDSequence() from bbepInitIO() for 7-colour panels. This mirrors upstream --
 * esp_generic.inl:36-37 and arduino_io.inl:28-29 carry the same two declarations for the same
 * reason. Note both do their own RST pulses; see the header comment. */
void bbepWakeUp(BBEPDISP *pBBEP);
void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);

/* SPI2 is the general-purpose host on every variant this target builds for (SPI1 is the flash
 * controller). VSPI_HOST is the pre-IDF-5 spelling and is not defined on any variant here, but
 * the fallback costs nothing and matches upstream's guard. */
#ifdef VSPI_HOST
#define OD_BBEP_SPI_HOST VSPI_HOST
#else
#define OD_BBEP_SPI_HOST SPI2_HOST
#endif

/* SPI state. Non-static file scope in upstream; static here because nothing outside this
 * translation unit has any business touching it.
 *
 * NO SHARED TRANSACTION STRUCT. Upstream keeps `static spi_transaction_t trans;` and memsets
 * it per call, which works only because every path is single-task polling transmit -- while
 * simultaneously advertising queue_size = 2, i.e. queued transactions that one shared struct
 * cannot support. Ours is a local in od_bbep_spi_write(), so the function is re-entrant and
 * queue_size can be raised later without a latent aliasing bug. */
static spi_device_handle_t s_spi = NULL;
static bool                s_spi_ready = false;

/* ------------------------------------------------------------------ GPIO primitives
 *
 * bb_ep.inl calls these directly for RST and BUSY, bypassing the byte path. Semantics
 * MATCH compat/arduino_compat.h deliberately:
 *
 *   - NO gpio_reset_pin(). Upstream's pinMode() calls it first, so bb_epaper's idea of
 *     "configure a pin" differed from the application's in the same firmware -- and
 *     gpio_reset_pin() re-enables the pad's default pull, which is not what a caller asking
 *     for a plain output wants.
 *   - Pin validity IS checked. Upstream guards only BUSY, so an unset DC/RST/CS pin (0xFF)
 *     reached gpio_config() as `1ULL << 255` -- undefined behaviour. This mirrors the shim's
 *     od_pin_valid().
 */
static inline bool od_bbep_pin_valid(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX && pin != 0xFF;
}

/* Configure a pin. Named od_bbep_* rather than pinMode() on purpose: nothing in bb_ep.inl or
 * bb_ep_gfx.inl calls pinMode (measured: 0), and defining it would re-export a third global
 * with that name. */
static void od_bbep_pin_output(int pin)
{
    if (!od_bbep_pin_valid(pin)) return;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << (uint32_t)pin);
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

static void od_bbep_pin_input(int pin)
{
    if (!od_bbep_pin_valid(pin)) return;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << (uint32_t)pin);
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

/* Signatures are fixed by bb_epaper.h:49-50 and must match exactly -- a mismatch here is how
 * the vendored tree's delay(int)/delay(long) ambiguity happened. */
void digitalWrite(int pin, int value)
{
    if (!od_bbep_pin_valid(pin)) return;
    gpio_set_level((gpio_num_t)pin, value ? 1 : 0);
}

int digitalRead(int pin)
{
    if (!od_bbep_pin_valid(pin)) return 0;
    return gpio_get_level((gpio_num_t)pin);
}

/* ------------------------------------------------------------------ SPI byte path */

/* Returns false on a genuine driver error instead of asserting.
 *
 * Upstream calls assert(ret==ESP_OK) here, on EVERY transfer. In shipped firmware that is a
 * reboot; and it is config-dependent -- with CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE
 * it becomes a silent no-op and SPI write failures vanish entirely. Neither is acceptable in
 * a layer whose failures are otherwise indistinguishable from a dead panel. Logging is rate-
 * limited to once per session because this is called thousands of times per frame. */

/* ------------------------------------------------------- cs_mode (bb_epaper 5dccfbb) ---
 *
 * The library gained a `cs_mode` field and DROPPED `iCS1Pin`. Dual-controller panels used to
 * be driven by mutating iCSPin between iCS1Pin and iCS2Pin around each write; now iCSPin IS
 * CS1, and cs_mode (CMD_CS1 / CMD_CS2 / CMD_CS1_CS2) selects which line(s) a transfer asserts.
 * That is what makes the 13.3" Spectra6 1200x1600 (reTerminal E1004) addressable at all -- it
 * is a split-controller panel, so CMD_CS1_CS2 has to assert BOTH.
 *
 * arduino_io.inl repeats this test at eight sites (bbepWriteCmd, bbepWriteData and
 * bbepWriteCmdData, entry and exit). This backend drives CS in exactly ONE place --
 * od_bbep_spi_write(), because spics_io_num is -1 and the peripheral never touches it -- so the
 * gating lives here once and every writer inherits it. Same semantics, one site to get wrong.
 *
 * cs_mode == 0 is treated as CMD_CS1. A BBEPDISP zeroed by memset (which is how both targets
 * create theirs) would otherwise assert NOTHING and the panel would sit silent. */
static inline void od_bbep_cs_assert(BBEPDISP *pBBEP, int level)
{
    const uint8_t mode = pBBEP->cs_mode ? pBBEP->cs_mode : (uint8_t)CMD_CS1;
    if (mode == CMD_CS1 || mode == CMD_CS1_CS2) {
        digitalWrite(pBBEP->iCSPin, level);
    }
    if (mode == CMD_CS2 || mode == CMD_CS1_CS2) {
        digitalWrite(pBBEP->iCS2Pin, level);
    }
}

/* The transfer itself, WITHOUT touching CS. Split out so bbepWriteCmdData() can hold one CS
 * assertion across a command byte and its payload -- see its comment. */
static bool od_bbep_spi_write_nocs(BBEPDISP *pBBEP, const uint8_t *pBuf, int iLen)
{
    (void)pBBEP;
    if (s_spi == NULL || pBuf == NULL || iLen <= 0) {
        return false;
    }
    bool ok = true;
    while (iLen > 0) {
        /* max_transfer_sz below is 4096; chunk under it. Upstream chunks at 4000 with a
         * "full duplex mode" comment while registering the device SPI_DEVICE_HALFDUPLEX --
         * three numbers nobody reconciled. One constant, used for both. */
        int l = (iLen > OD_BBEP_MAX_XFER) ? OD_BBEP_MAX_XFER : iLen;
        spi_transaction_t t = {};          /* local: re-entrant, unlike upstream's shared static */
        t.length    = (size_t)l * 8;       /* bits */
        t.rxlength  = 0;
        t.tx_buffer = pBuf;
        esp_err_t ret = spi_device_polling_transmit(s_spi, &t);
        if (ret != ESP_OK) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                ESP_LOGE(OD_BBEP_TAG, "SPI transmit failed (%s); panel output is incomplete. "
                                      "Further occurrences suppressed.", esp_err_to_name(ret));
            }
            ok = false;
            break;
        }
        iLen -= l;
        pBuf += l;
    }
    return ok;
}

static bool od_bbep_spi_write(BBEPDISP *pBBEP, const uint8_t *pBuf, int iLen)
{
    if (s_spi == NULL || pBuf == NULL || iLen <= 0) {
        return false;
    }
    od_bbep_cs_assert(pBBEP, LOW);
    const bool ok = od_bbep_spi_write_nocs(pBBEP, pBuf, iLen);
    od_bbep_cs_assert(pBBEP, HIGH);
    return ok;
}

/* ------------------------------------------------------- data streams (OUR extension)
 *
 * See panel/od_bbep_stream.h for why these exist. In short: the E1004 dual-CS panel streams a
 * half-plane with CS held, and it used to do that through the Arduino SPI object, which
 * registers a SECOND device on the host this file already owns. These give it the bus it was
 * always writing to.
 *
 * Deliberately NOT expressed as bbepWriteData(): that deasserts CS per call, which is
 * bb_epaper's per-row framing and not the E1004's per-half-plane framing. Changing the wire
 * behaviour and changing the device owner in one step would leave a hardware failure with two
 * candidate causes. */
static int s_stream_cs = -1;

bool od_bbep_stream_begin(int cs_pin)
{
    if (s_spi == NULL) {
        return false;
    }
    /* A stream left open by an aborted transfer must not strand CS low on the previous pin --
     * e1004_advance_to_cs2() switches chip selects mid-plane. */
    od_bbep_stream_end();
    if (!od_bbep_pin_valid(cs_pin)) {
        return false;
    }
    s_stream_cs = cs_pin;
    digitalWrite(s_stream_cs, LOW);
    return true;
}

bool od_bbep_stream_write(const uint8_t *buf, int len)
{
    if (s_spi == NULL || buf == NULL || len <= 0) {
        return false;
    }
    /* Same chunking as od_bbep_spi_write(), same local transaction struct (re-entrant), and
     * deliberately NO CS handling -- begin() owns that for the whole stream. */
    while (len > 0) {
        int l = (len > OD_BBEP_MAX_XFER) ? OD_BBEP_MAX_XFER : len;
        spi_transaction_t t = {};
        t.length    = (size_t)l * 8;
        t.rxlength  = 0;
        t.tx_buffer = buf;
        esp_err_t ret = spi_device_polling_transmit(s_spi, &t);
        if (ret != ESP_OK) {
            static bool stream_warned = false;
            if (!stream_warned) {
                stream_warned = true;
                ESP_LOGE(OD_BBEP_TAG, "SPI stream transmit failed (%s); panel output is "
                                      "incomplete. Further occurrences suppressed.",
                         esp_err_to_name(ret));
            }
            return false;
        }
        len -= l;
        buf += l;
    }
    return true;
}

void od_bbep_stream_end(void)
{
    if (s_stream_cs < 0) {
        return;
    }
    digitalWrite(s_stream_cs, HIGH);
    s_stream_cs = -1;
}

/* ------------------------------------------------------------------ contract: command/data */

void bbepWriteCmd(BBEPDISP *pBBEP, uint8_t cmd)
{
    if (!pBBEP->is_awake) {
        /* Asleep controllers ignore commands. bbepWakeUp() lives in bb_ep.inl and does the
         * RST pulse itself; this is upstream's ordering, preserved. */
        bbepWakeUp(pBBEP);
        pBBEP->is_awake = 1;
    }
    digitalWrite(pBBEP->iDCPin, LOW);
    delay(1);
    od_bbep_spi_write(pBBEP, &cmd, 1);
    digitalWrite(pBBEP->iDCPin, HIGH);   /* leave data mode as the default */
}

void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen)
{
    if (pBBEP->iFlags & BBEP_CS_EVERY_BYTE) {
        /* Some controllers latch on the CS edge and need one transaction per byte. */
        for (int i = 0; i < iLen; i++) {
            od_bbep_spi_write(pBBEP, &pData[i], 1);
        }
    } else {
        od_bbep_spi_write(pBBEP, pData, iLen);
    }
}

/* NEW IN THE CONTRACT at bb_epaper 5dccfbb: bb_ep.inl calls this directly (two sites), so a
 * backend without it no longer links. Modelled on arduino_io.inl's version -- command byte with
 * DC low, then the payload with DC high, all inside ONE CS assertion. That single-assertion
 * property is the point: splitting it into bbepWriteCmd() + bbepWriteData() would toggle CS in
 * between, and controllers that latch on the CS edge would see two transactions where the
 * library intends one. */
void bbepWriteCmdData(BBEPDISP *pBBEP, uint8_t cmd, uint8_t *pData, int iLen)
{
    if (!pBBEP->is_awake) {
        bbepWakeUp(pBBEP);
        pBBEP->is_awake = 1;
    }
    od_bbep_cs_assert(pBBEP, LOW);
    digitalWrite(pBBEP->iDCPin, LOW);
    od_bbep_spi_write_nocs(pBBEP, &cmd, 1);
    digitalWrite(pBBEP->iDCPin, HIGH);
    if (pData != NULL && iLen > 0) {
        od_bbep_spi_write_nocs(pBBEP, pData, iLen);
    }
    od_bbep_cs_assert(pBBEP, HIGH);
}

void bbepCMD2(BBEPDISP *pBBEP, uint8_t cmd1, uint8_t cmd2)
{
    bbepWriteCmd(pBBEP, cmd1);
    bbepWriteData(pBBEP, &cmd2, 1);
}

void bbepSetCS2(BBEPDISP *pBBEP, uint8_t cs)
{
    /* No iCS1Pin assignment: the field was REMOVED at bb_epaper 5dccfbb because iCSPin is
     * CS1. Keeping a stale copy here is how the two would drift. */
    pBBEP->iCS2Pin = cs;
    od_bbep_pin_output(cs);
    digitalWrite(cs, HIGH);   /* second controller deselected until addressed */
}

/* ------------------------------------------------------------------ contract: lifecycle */

/* Release the bus and device. NOT AN UPSTREAM FUNCTION -- upstream has no teardown at all,
 * which is the root cause of two separate defects rather than a missing convenience:
 *
 *   1. bbepInitIO() called spi_bus_initialize() on every cold bring-up with nothing ever
 *      freeing the bus, so bring-up #2 could only assert.
 *   2. Guarding that init behind an "already done" flag (the obvious fix) breaks the panel
 *      instead, because project code revokes the pad routing between bring-ups:
 *      configureDisplayPinsLowPower() -> pinMode() -> gpio_config() takes SCLK/MOSI back as
 *      plain GPIO, and spi_bus_initialize() is the only call that re-attaches them. Skipping
 *      it leaves a healthy peripheral wired to nothing -- every transfer returns ESP_OK and
 *      the screen never changes.
 *
 * With a real teardown, bbepInitIO() can do the honest thing: release, then re-initialise.
 * Call this from the panel power-down path (epdSessionForceOffLocked()); it is idempotent, so
 * a redundant call is free. */
void bbepDeInitIO(void)
{
    if (!s_spi_ready) {
        return;
    }
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    esp_err_t ret = spi_bus_free(OD_BBEP_SPI_HOST);
    if (ret != ESP_OK) {
        /* Another driver still holds a device on this host -- the E1004 dual-CS path opens one
         * through compat/SPI.h. Not fatal, but it means the next init cannot re-attach the
         * pins, so it must not be silent. */
        ESP_LOGW(OD_BBEP_TAG, "spi_bus_free failed (%s); another device is still attached",
                 esp_err_to_name(ret));
    }
    s_spi_ready = false;
}

void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS,
                uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed)
{
    pBBEP->iDCPin   = u8DC;
    pBBEP->iCSPin   = u8CS;
    pBBEP->iMOSIPin = u8MOSI;
    pBBEP->iCLKPin  = u8SCK;
    pBBEP->iRSTPin  = u8RST;
    pBBEP->iBUSYPin = u8BUSY;
    pBBEP->iSpeed   = u32Speed;

    /* Control pins first, so the reset pulse below drives a configured pad. Each guarded:
     * upstream guards only BUSY and would hand 0xFF to gpio_config() as `1ULL << 255`. */
    od_bbep_pin_output(pBBEP->iDCPin);
    od_bbep_pin_output(pBBEP->iCSPin);
    digitalWrite(pBBEP->iCSPin, HIGH);   /* CS is driven by this file, never by the peripheral */
    if (pBBEP->iBUSYPin != 0xFF) {
        od_bbep_pin_input(pBBEP->iBUSYPin);
    }

    /* Hardware reset. Kept identical to upstream's esp_generic.inl (LOW 100 ms, HIGH 100 ms)
     * rather than adopting arduino_io.inl's "drive HIGH, no pulse": changing panel reset
     * timing is not this file's business, and doing it in the same change that swaps the
     * backend would make a regression impossible to attribute. Note bb_ep.inl pulses RST
     * again in bbepWakeUp() and bbepSendCMDSequence() on every platform. */
    if (pBBEP->iRSTPin != 0xFF) {
        od_bbep_pin_output(pBBEP->iRSTPin);
        digitalWrite(pBBEP->iRSTPin, LOW);
        delay(100);
        digitalWrite(pBBEP->iRSTPin, HIGH);
        delay(100);
    }

    /* Release before re-initialising. See bbepDeInitIO() for why skipping the re-init is not
     * an option: the pad routing does not survive the power-down. */
    bbepDeInitIO();

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num     = (u8MOSI == 0xFF) ? -1 : (int)u8MOSI;
    buscfg.miso_io_num     = -1;               /* write-only; the panel returns nothing */
    buscfg.sclk_io_num     = (u8SCK == 0xFF) ? -1 : (int)u8SCK;
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = OD_BBEP_MAX_XFER;

    esp_err_t ret = spi_bus_initialize(OD_BBEP_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* The free above did not take. Degraded, not fatal: the pins are whatever the previous
         * owner left them as, which on this target means the panel is about to go silent. This
         * is the single most useful warning in the file. */
        ESP_LOGW(OD_BBEP_TAG, "SPI bus already initialised; SCLK/MOSI pin routing NOT refreshed "
                              "-- panel writes will be accepted and go nowhere");
    } else if (ret != ESP_OK) {
        ESP_LOGE(OD_BBEP_TAG, "spi_bus_initialize failed (%s); panel unavailable",
                 esp_err_to_name(ret));
        return;   /* no assert: a dead panel must not take the radio and the host link with it */
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = (int)u32Speed;
    devcfg.mode           = 0;
    devcfg.spics_io_num   = -1;   /* CS driven in od_bbep_spi_write() */
    devcfg.queue_size     = 1;    /* polling transmits only; see the note on s_spi */
    devcfg.flags          = SPI_DEVICE_HALFDUPLEX;

    ret = spi_bus_add_device(OD_BBEP_SPI_HOST, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(OD_BBEP_TAG, "spi_bus_add_device failed (%s); panel unavailable",
                 esp_err_to_name(ret));
        s_spi = NULL;
        return;
    }
    s_spi_ready = true;

    /* Upstream sends pInitFull here for 7-colour panels only, because those need the panel
     * resolution before they will accept data. Preserved verbatim -- display_service.cpp sends
     * the init sequence itself for every other panel family. */
    if (pBBEP->iFlags & BBEP_7COLOR) {
        pBBEP->is_awake = 1;
        bbepSendCMDSequence(pBBEP, pBBEP->pInitFull);
    }
}

#endif /* OD_BBEP_IDF_IO_INL */
