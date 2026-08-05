/* od_bbep_zephyr.h -- this target's bb_epaper surface: raw BBEPDISP, no C++ class.
 *
 * WHY THIS EXISTS. The vendored BBEPAPER C++ class is not compiled on this target (its 63
 * method bodies live in third_party/bb_epaper/src/bb_epaper.cpp, which panel/od_bbep_zephyr.cpp
 * replaces). That is the ESP32's model -- targets/esp32-idf drives a raw BBEPDISP through the
 * bare C functions -- and adopting it here means bb_epaper is integrated the same way on both
 * targets, with ZERO edits to the vendored tree.
 *
 * TWO PROBLEMS THIS HEADER SOLVES:
 *
 * 1. bb_epaper.h publicly declares only bbepWriteCmd/bbepWriteData/bbepCMD2 (bb_epaper.h:673).
 *    The rest of the core -- bbepSetPanelType, bbepRefresh, bbepFill and friends -- are
 *    externally visible definitions inside bb_ep.inl with no public declaration. Callers
 *    outside the glue TU need declarations, and they belong in target-owned code rather than
 *    in a patch to the vendored header.
 *
 * 2. Two class methods did MORE than forward, and dropping the class would have silently
 *    changed what goes down the wire. They are replicated below, faithfully, as helpers.
 */
#ifndef OD_BBEP_ZEPHYR_H
#define OD_BBEP_ZEPHYR_H

#include "bb_epaper.h"

/* NO extern "C" HERE, deliberately. The vendored core is compiled as C++ (bb_ep.inl is
 * included from a .cpp and declares nothing extern "C"), so wrapping these in C linkage
 * produces "conflicting declaration ... with 'C' linkage" against the definitions. This target
 * is C++ throughout, so plain C++ linkage is both correct and what the vendored tree expects. */

/* ------------------------------------------------------------------ the composite helpers ---
 *
 * THESE ARE NOT CONVENIENCE WRAPPERS. Each reproduces a vendored class method that performed
 * extra panel initialisation beyond the obvious bare-C call. Replacing either with its
 * one-line equivalent is a wire-behaviour change on shipped hardware, and it would not show up
 * in any build or host test -- only on a 4-colour, 7-colour or split-controller panel.
 */

/* BBEPAPER::wake() (bb_epaper.cpp:713).
 *
 * bbepWakeUp() is only the first half. For BBEP_7COLOR or BBEP_4COLOR panels the class then
 * sends the FULL init sequence -- those controllers cannot accept data until it has been sent
 * -- and for BBEP_SPLIT_BUFFER (dual-cable) panels it repeats that on the second controller,
 * swapping iCSPin to iCS2Pin and restoring iCS1Pin afterwards. A plain bbepWakeUp() leaves a
 * 4-/7-colour panel un-initialised and a split panel half-initialised. */
void od_bbep_wake(BBEPDISP *pBBEP);

/* BBEPAPER::sendPanelInitFull() -- a method Firmware_NRF54 ADDED to its vendored copy, which
 * upstream does not have (Firmware_NRF54 bb_epaper.cpp:732). Replicated here instead of
 * re-adding it to the vendored class, so the vendored tree stays byte-identical to upstream.
 *
 * The CS1 restoration is part of the contract, not tidy-up: leaving iCSPin on the second
 * controller silently sends every subsequent command to the wrong half of the panel. */
void od_bbep_send_panel_init_full(BBEPDISP *pBBEP);

/* --------------------------------------------------------- declarations for the bare core ---
 *
 * Defined in bb_ep.inl (via od_bbep_zephyr.cpp) or in od_bbep_zephyr_io.inl. Declared here
 * because the vendored header does not declare them.
 *
 * SIGNATURES ARE COPIED FROM THE DEFINITIONS, NOT GUESSED. Getting a return type wrong here is
 * not a warning -- C++ rejects it as an ambiguating declaration, which is the one piece of luck
 * in this arrangement. Three were wrong on the first pass (bbepFill, bbepIsBusy, bbepStartWrite
 * all return void/bool, not int) and the compiler caught every one.
 */
int  bbepSetPanelType(BBEPDISP *pBBEP, int iPanel);
void bbepSetRotation(BBEPDISP *pBBEP, int iRotation);
void bbepFill(BBEPDISP *pBBEP, unsigned char ucColor, int iPlane);
int  bbepRefresh(BBEPDISP *pBBEP, int iMode);
void bbepSleep(BBEPDISP *pBBEP, int bDeep);
void bbepWakeUp(BBEPDISP *pBBEP);
void bbepWaitBusy(BBEPDISP *pBBEP);
bool bbepIsBusy(BBEPDISP *pBBEP);
void bbepSetAddrWindow(BBEPDISP *pBBEP, int x, int y, int cx, int cy);
void bbepStartWrite(BBEPDISP *pBBEP, int iPlane);
void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);
void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS,
                uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed);

#endif /* OD_BBEP_ZEPHYR_H */
