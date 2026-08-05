/* od_bbep_zephyr.cpp -- the bb_epaper translation unit for the Nordic/Zephyr target.
 *
 * REPLACES third_party/bb_epaper/src/bb_epaper.cpp, which this build excludes. Directly
 * modelled on targets/esp32-idf/panel/od_bbep.cpp; the reasoning there applies here unchanged,
 * and the two targets deliberately solve this the same way.
 *
 * WHY AN ADAPTER RATHER THAN A PATCH. Firmware_NRF54 selected its backend by editing the
 * vendored bb_epaper.cpp's #ifdef chain -- adding `#elif defined(TARGET_NRF54) ||
 * defined(CONFIG_ZEPHYR)` arms -- and by patching bb_ep.inl. Carrying that here would mean two
 * more edits to vendored files, two more entries on third_party/NOTICE.md's "re-verify on every
 * bump" list, and a selector macro the build has to keep defining. This file gets the same
 * result with ZERO vendored edits, so third_party/bb_epaper stays byte-identical to upstream
 * for every target at once.
 *
 * THE VENDORED bbepWaitBusy() IS USED, AND THAT IS A DELIBERATE DIVERGENCE FROM THE SOURCE
 * REPO. Firmware_NRF54 overrode it with nrf54_bbep_busy.inl, which required a local patch to
 * bb_ep.inl (`#if !defined(TARGET_NRF54) && !defined(CONFIG_ZEPHYR)` around the vendored
 * definition) to avoid a duplicate symbol -- an undocumented edit to a vendored file.
 *
 * The override turns out to be unnecessary. The vendored bbepWaitBusy() needs delay(),
 * digitalRead() and bbepLightSleep(); od_bbep_zephyr_io.inl supplies the first two and
 * bb_ep.inl the third, so the generic implementation works as-is on this target. The two
 * implementations are the same algorithm -- same 5 s / 30 s timeouts, same UC81xx polarity
 * inversion -- differing only in whether the pin read goes through digitalRead() or
 * nrf54_gpio_read() directly, and digitalRead() here IS nrf54_gpio_read().
 *
 * CORRECTION (validated 2026-08-05): an earlier version of this comment claimed the vendored
 * version "additionally honours bLightSleep", so the busy-wait might now enter light sleep
 * where it previously spun. THAT WAS WRONG. bbepLightSleep() is `#ifdef ARDUINO_ARCH_ESP32`
 * only; off ESP32 it is `(void)bLightSleep; delay(u32Millis)` (bb_ep.inl:3962-3975). On Zephyr
 * the two implementations poll identically -- same 10 ms + 1 ms settle, same 20 ms poll, same
 * 5 s / 30 s timeouts, same UC81xx polarity -- and digitalRead() here IS nrf54_gpio_read().
 *
 * The only real difference is on TIMEOUT: the source override re-read BUSY and logged; the
 * vendored one's warning is guarded by ESP_PLATFORM, so this build is silent. Both then return
 * void and continue as if ready. A lost diagnostic, not a timing change.
 *
 * INCLUDE ORDER IS LOAD-BEARING and must not be "tidied":
 *
 *   bb_epaper.h              declares BBEPDISP and the exact primitive signatures the backend
 *                            must match. The delay(int)/delay(long) mismatch this header's
 *                            OD-PATCH exists for is precisely the trap here.
 *   od_bbep_zephyr_io.inl    OUR backend -- must precede bb_ep.inl, which calls into it.
 *   bb_ep.inl                all panel logic: init sequences, LUTs, chip quirks, RST pulses and
 *                            the BUSY poll. Unmodified.
 *   bb_ep_gfx.inl            drawing; pulls in g5dec.inl itself.
 *
 * The unused BBEPAPER C++ class from the vendored .cpp is dropped, exactly as the ESP32 adapter
 * drops it: this project drives a raw `BBEPDISP` through the bare C functions. Group5.cpp stays
 * a separate translation unit and is still compiled.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bb_epaper.h"

/* Our backend. Target-owned (see the file header), so third_party/ takes no edit. */
#include "od_bbep_zephyr_io.inl"

/* Vendored, unmodified. */
#include "bb_ep.inl"
#include "bb_ep_gfx.inl"

/* ------------------------------------------------------------------ the composite helpers ---
 *
 * See od_bbep_zephyr.h for why these are not one-liners. Both are transcribed from the
 * vendored/source class methods so the wire behaviour is unchanged by dropping the class.
 */
#include "od_bbep_zephyr.h"

void od_bbep_wake(BBEPDISP *pBBEP)
{
    /* Transcribed from BBEPAPER::wake(), third_party/bb_epaper/src/bb_epaper.cpp:713. */
    bbepWakeUp(pBBEP);
    if (pBBEP->iFlags & (BBEP_7COLOR | BBEP_4COLOR)) {
        /* These controllers cannot accept data until the full init sequence has been sent. */
        bbepSendCMDSequence(pBBEP, pBBEP->pInitFull);
        if (pBBEP->iFlags & BBEP_SPLIT_BUFFER) {   /* dual-cable EPD: second controller */
            pBBEP->iCSPin = pBBEP->iCS2Pin;
            bbepSendCMDSequence(pBBEP, pBBEP->pInitFull);
            pBBEP->iCSPin = pBBEP->iCS1Pin;        /* restore: not tidy-up, see the header */
        }
    }
}

void od_bbep_send_panel_init_full(BBEPDISP *pBBEP)
{
    /* Transcribed from the method Firmware_NRF54 added to its vendored copy,
     * Firmware_NRF54/third_party/bb_epaper/src/bb_epaper.cpp:732. Note the guard differs from
     * od_bbep_wake() above -- 7COLOR only, not 7COLOR|4COLOR -- and that asymmetry is
     * reproduced deliberately rather than harmonised. */
    bbepSendCMDSequence(pBBEP, pBBEP->pInitFull);
    if (pBBEP->iFlags & BBEP_7COLOR) {
        if (pBBEP->iFlags & BBEP_SPLIT_BUFFER) {
            pBBEP->iCSPin = pBBEP->iCS2Pin;
            bbepSendCMDSequence(pBBEP, pBBEP->pInitFull);
            pBBEP->iCSPin = pBBEP->iCS1Pin;
        }
    }
}
