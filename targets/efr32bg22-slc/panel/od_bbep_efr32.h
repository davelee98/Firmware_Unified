/* od_bbep_efr32.h -- this target's bb_epaper surface: raw BBEPDISP, no C++ class.
 *
 * THIRD TARGET ON THIS PATTERN, and deliberately not a third variation of it. See
 * targets/esp32-idf/panel/od_bbep.cpp and targets/nordic-zephyr/panel/od_bbep_zephyr.h -- the
 * reasoning there applies here unchanged, and all three now integrate bb_epaper the same way,
 * so a re-vendor has one integration to re-check rather than three.
 *
 * The vendored BBEPAPER C++ class is not compiled here: its method bodies live in
 * third_party/bb_epaper/src/bb_epaper.cpp, which panel/od_bbep_efr32.cpp replaces.
 *
 * TWO PROBLEMS THIS HEADER SOLVES:
 *
 * 1. bb_epaper.h publicly declares only bbepWriteCmd/bbepWriteData/bbepCMD2. The rest of the
 *    core is externally visible definitions inside bb_ep.inl with no public declaration.
 *    Callers outside the glue TU need declarations, and they belong in target-owned code rather
 *    than in a patch to the vendored header.
 * 2. Two class methods did MORE than forward. Dropping the class without replicating them
 *    changes what goes down the wire, and nothing in a build or host test would notice.
 */
#ifndef OD_BBEP_EFR32_H
#define OD_BBEP_EFR32_H

#include "bb_epaper.h"

/* No extern "C": the vendored core is compiled as C++ (bb_ep.inl is included from a .cpp and
 * declares nothing extern "C"), so C linkage here conflicts with those definitions. */

/* ------------------------------------------------------------------ the composite helpers ---
 *
 * NOT CONVENIENCE WRAPPERS. Each reproduces a vendored class method that performed extra panel
 * initialisation beyond the obvious bare-C call. Replacing either with its one-line equivalent
 * is a wire-behaviour change on shipped hardware, visible only on a 4-colour, 7-colour or
 * split-controller panel.
 */

/* BBEPAPER::wake(). bbepWakeUp() is only the first half: for BBEP_7COLOR or BBEP_4COLOR panels
 * the class then sends the FULL init sequence, because those controllers accept no data until
 * it has been sent. A plain bbepWakeUp() leaves such a panel un-initialised.
 *
 * The split-controller CS dance the older library needed is GONE, and its absence is correct
 * rather than an omission: bb_epaper 5dccfbb replaced manual iCSPin mutation with cs_mode, and
 * iCS1Pin -- which the old restore step used -- no longer exists. Upstream's own wake() dropped
 * the same block in the same revision. */
void od_bbep_wake(BBEPDISP *pBBEP);

/* BBEPAPER::sendPanelInitFull() -- a method Firmware_NRF54 ADDED to its vendored copy, which
 * upstream does not have. This is the THIRD target to depend on it (ESP32 does not; it drives
 * the bare functions directly). Replicated here rather than re-added to the vendored class, so
 * third_party/ stays byte-identical to upstream. */
void od_bbep_send_panel_init_full(BBEPDISP *pBBEP);

/* --------------------------------------------------------- declarations for the bare core ---
 *
 * Defined in bb_ep.inl (via od_bbep_efr32.cpp) or in od_bbep_efr32_io.inl. SIGNATURES COPIED
 * FROM THE DEFINITIONS, NOT GUESSED -- a wrong return type here is an ambiguating declaration
 * that C++ rejects outright, which is the one piece of luck in this arrangement.
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
void bbepSetCS2(BBEPDISP *pBBEP, uint8_t cs);

#endif /* OD_BBEP_EFR32_H */
