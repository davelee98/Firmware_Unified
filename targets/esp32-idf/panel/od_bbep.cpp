/* od_bbep.cpp -- the bb_epaper translation unit for this target.
 *
 * REPLACES third_party/bb_epaper/src/bb_epaper.cpp, which the build excludes. That file is 827
 * lines, of which only the first 52 are glue -- standard includes, the *_io.inl selection
 * #ifdef chain, then bb_ep.inl and bb_ep_gfx.inl. Everything from line 53 (`#ifdef __cplusplus`)
 * is the BBEPAPER C++ class, which this project never references: display_service.cpp drives a
 * raw `BBEPDISP bbep;` through the bare C functions instead.
 *
 * TWO REASONS THIS EXISTS rather than a patch to bb_epaper.cpp's #ifdef chain, which is how
 * Firmware_NRF54 selects its own backend:
 *
 * 1. ZERO EDITS TO VENDORED FILES. bb_epaper.h, bb_ep.inl and bb_ep_gfx.inl are included
 *    unmodified. There is no fifth OD-PATCH to re-apply on the next re-vendor and no selector
 *    macro to define in the build. third_party/NOTICE.md's "re-verify on every bump" list does
 *    not grow.
 *
 * 2. IT DROPS THE UNUSED C++ CLASS, which was the only consumer of pinMode() (13 calls) and
 *    millis() (6). That is why od_bbep_idf_io.inl defines neither, and why the backend contract
 *    here is nine functions rather than eleven. See docs/BBEPAPER_IO_BACKENDS.md §2.
 *
 * INCLUDE ORDER IS LOAD-BEARING and must not be "tidied":
 *
 *   bb_epaper.h            declares BBEPDISP and the primitive signatures the backend must
 *                          match exactly (bb_epaper.h:47-50). Getting one wrong is how the
 *                          vendored tree's delay(int)/delay(long) ambiguity happened.
 *   od_bbep_idf_io.inl     OUR backend -- must precede bb_ep.inl, which calls into it.
 *   bb_ep.inl              all panel logic: init sequences, LUTs, chip quirks, the RST pulses
 *                          and the BUSY poll loop. Unmodified, and deliberately so.
 *   bb_ep_gfx.inl          drawing; pulls in g5dec.inl itself.
 *
 * Group5.cpp stays a separate translation unit and is still compiled -- it provides the G5
 * codec and includes g5enc.inl/g5dec.inl on its own. The FastEPD-vs-bb_epaper Group5 collision
 * handling in main/CMakeLists.txt is unaffected by this file.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bb_epaper.h"
#include "od_bbep_stream.h"

#include "od_bbep_idf_io.inl"

#include "bb_ep.inl"
#include "bb_ep_gfx.inl"
