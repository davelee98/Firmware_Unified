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
 * WHAT THAT COSTS, STATED RATHER THAN GLOSSED: this is a behaviour change on a SHIPPED target,
 * and it is not hardware-verified. The vendored version additionally honours bLightSleep, which
 * the nRF54 override did not -- so a busy-wait on this target may now enter light sleep between
 * polls where it previously spun. That should be a power improvement and is the vendored
 * library's intent, but it is a real difference in timing behaviour on a panel path, and it
 * belongs on the hardware checklist before this target is called working.
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
