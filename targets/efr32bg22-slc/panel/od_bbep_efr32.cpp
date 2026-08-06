/* od_bbep_efr32.cpp -- the bb_epaper translation unit for the EFR32BG22 target.
 *
 * REPLACES third_party/bb_epaper/src/bb_epaper.cpp, which this build excludes. Third target on
 * this pattern; see targets/esp32-idf/panel/od_bbep.cpp for the original reasoning and
 * targets/nordic-zephyr/panel/od_bbep_zephyr.cpp for the second application of it.
 *
 * WHY AN ADAPTER RATHER THAN A PATCH. Firmware_Silabs selected its backend by editing the
 * vendored bb_epaper.cpp's #ifdef chain (`#elif defined(__SILABS_BG22__)`) and by patching
 * bb_ep.inl to guard out bbepWaitBusy(). Carrying either here would mean edits to a vendored
 * tree three targets now share, and two more entries on third_party/NOTICE.md's "re-verify on
 * every bump" list -- a list that just proved its worth: the 5dccfbb re-vendor retired one of
 * three patches and broke the backend contract twice.
 *
 * THE VENDORED bbepWaitBusy() IS USED; silabs_bbep_busy.inl is NOT ported. That override only
 * existed because the source patched bb_ep.inl to suppress the vendored definition. It is
 * unnecessary: the vendored implementation needs delay(), digitalRead() and bbepLightSleep(),
 * and od_bbep_efr32_io.inl supplies the first two while bb_ep.inl supplies the third. Both use
 * the same 5 s / 30 s timeouts and the same UC81xx polarity inversion. This is the same call
 * made for the Nordic target, for the same reason.
 *
 * WHAT THAT COSTS, stated rather than glossed: on TIMEOUT the source override logged and this
 * one does not -- the vendored warning is guarded by ESP_PLATFORM, so this build is silent when
 * a panel's BUSY line never releases. Both then return void and continue as if ready. A lost
 * diagnostic, not a timing change. It is worth restoring as a target-side wrapper if BG22 panel
 * bring-up ever needs it.
 *
 * INCLUDE ORDER IS LOAD-BEARING and must not be "tidied":
 *
 *   bb_epaper.h            declares BBEPDISP and the exact primitive signatures the backend
 *                          must match -- including the delay(long) OD-PATCH, which is why the
 *                          ported backend changed delay(int).
 *   od_bbep_efr32_io.inl   OUR backend -- must precede bb_ep.inl, which calls into it.
 *   bb_ep.inl              all panel logic: init sequences, LUTs, chip quirks, RST pulses and
 *                          the BUSY poll. Unmodified.
 *   bb_ep_gfx.inl          drawing; pulls in g5dec.inl itself.
 *
 * The unused BBEPAPER C++ class is dropped, as on the other two targets: this project drives a
 * raw BBEPDISP through the bare C functions.
 *
 * SIZE IS A REAL RISK HERE AND IS NOT YET MEASURED. This is the 32 KB-RAM target, and it is the
 * first to compile bb_ep.inl in full. Flash and RAM after this lands must be compared against
 * the source repo's baseline (text 236316 / data 492 / bss 31792); if it does not fit, that is
 * a finding to report, not something to squeeze past.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bb_epaper.h"

/* Our backend. Target-owned (see od_bbep_efr32_io.inl's header), so third_party/ takes no edit. */
#include "od_bbep_efr32_io.inl"

/* Vendored, unmodified. */
#include "bb_ep.inl"
#include "bb_ep_gfx.inl"

#include "od_bbep_efr32.h"

/* ------------------------------------------------------------------ the composite helpers ---
 *
 * Transcribed from the vendored/source class methods so that dropping the class does not change
 * what goes down the wire. See od_bbep_efr32.h for why these are not one-liners.
 */

void od_bbep_wake(BBEPDISP *pBBEP)
{
    /* Transcribed from BBEPAPER::wake(), third_party/bb_epaper/src/bb_epaper.cpp. */
    bbepWakeUp(pBBEP);
    if (pBBEP->iFlags & (BBEP_7COLOR | BBEP_4COLOR)) {
        /* These controllers cannot accept data until the full init sequence has been sent. */
        bbepSendCMDSequence(pBBEP, pBBEP->pInitFull);
    }
}

void od_bbep_send_panel_init_full(BBEPDISP *pBBEP)
{
    /* Transcribed from the method Firmware_NRF54 added to its vendored copy. The CS1/CS2 swap
     * it used to carry depended on iCS1Pin, removed at 5dccfbb in favour of cs_mode: a caller
     * needing both controllers programmed sets cs_mode = CMD_CS1_CS2 before calling, and the
     * backend asserts both lines. It is no longer this function's business. */
    bbepSendCMDSequence(pBBEP, pBBEP->pInitFull);
}
