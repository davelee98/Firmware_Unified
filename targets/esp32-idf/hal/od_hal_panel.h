/* od_hal_panel -- the display-specific HAL.
 *
 * Signatures are docs/SHARED_API_DESIGN.md § od_hal_panel, including all five fixes that
 * section applies to display_fastepd.h's shape. Written to that contract now so the eventual
 * promotion to shared/hal is a repoint rather than a rewrite -- the same bet od_hal_time and
 * od_hal_nvs took, and the same one that has already paid off twice.
 *
 * THIS IS THE ONE INTERFACE WITH A FUNCTION-POINTER TABLE, and it is a deliberate exception
 * rather than an oversight. CLAUDE.md forbids vtables in shared code -- "never C++ virtual
 * classes, and no function-pointer vtables except the one deliberate od_panel_ops exception".
 * It is earned here because a SINGLE TARGET legitimately has two panel backends selected at
 * RUNTIME from parsed config: bb_epaper (streaming, no framebuffer) and FastEPD (buffered,
 * blits a full frame). Link-time binding cannot express that; every other HAL in this target
 * is link-time bound and must stay so.
 *
 * ---------------------------------------------------------------------------------------
 * THE FIVE CORRECTIONS TO display_fastepd.h, restated where they bind:
 *
 *   1. ALL C LINKAGE. display_fastepd.h wraps 2 of 16 functions in extern "C"; this file wraps
 *      everything, because two of the three targets this must eventually serve are C-only.
 *   2. A CAPABILITY QUERY (od_panel_caps_t), so the core asks instead of scattering
 *      color-geometry / backend / panel quirks
 *      across command handlers -- and so THE CORE, not the panel driver, decides whether 0x76
 *      is offered. It captures the streaming-vs-framebuffer split explicitly; the core must
 *      assume neither.
 *   3. NO BLOCKING REFRESH. refresh_start() returns immediately and refresh_busy() is polled.
 *      This replaces fastepd_wait_refresh(timeout_sec) and the Silabs target's 60 s in-callback
 *      block. A panel refresh can take 30 s on a 7-colour panel; holding the loop task for that
 *      is what the task watchdog exists to catch.
 *   4. SYMMETRIC ERROR REPORTING. Every data path returns int. FastEPD's full-frame write
 *      returns void and silently truncates while its partial path returns bool -- one of those
 *      had to win, and silence is not a defensible default on a transfer path.
 *   5. PLANE ORDER IS NORMALISED HERE. bb_epaper writes the first (old) plane to its PLANE_1
 *      (display_service.cpp's partial path does exactly this); FastEPD maps plane 0 to
 *      previousBuffer, i.e. natural order. Left alone, one backend inverts every partial
 *      update. THE CONTRACT IS: PLANE 0 IS THE OLD PLANE. Backends translate; the core never
 *      learns which vendor is underneath.
 * ---------------------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DisplayConfig;
struct SystemConfig;

/* Core types, from parsed config. NEVER a vendor enum: the whole point is that shared/core can
 * name a pixel format without bb_epaper's or FastEPD's headers being reachable. */
typedef enum {
    OD_PIX_1BPP = 0,
    OD_PIX_2BPP,
    OD_PIX_4GRAY,
    OD_PIX_BWR,
    OD_PIX_BWY
} od_pixfmt_t;

/* Values match display_fastepd.h's refresh_mode ints (0/1/2) so the FastEPD backend passes
 * them straight through and a mis-ordering cannot silently become a different refresh. */
typedef enum {
    OD_REFRESH_FULL    = 0,
    OD_REFRESH_FAST    = 1,
    OD_REFRESH_PARTIAL = 2
} od_refresh_t;

/* Negative on failure, everywhere, per correction 4. */
#define OD_PANEL_OK        0
#define OD_PANEL_ERR      (-1)   /* the panel or its bus refused */
#define OD_PANEL_EINVAL   (-2)   /* bad argument, or no session open */
#define OD_PANEL_ENOTSUP  (-3)   /* this backend does not implement this operation */

typedef struct {
    uint16_t    width, height;
    od_pixfmt_t fmt;
    uint8_t     plane_count;        /* direct-stream parts: 2 for BWR / BWY / GRAY4 */
    /* false = streaming sink (bb_epaper writes straight to controller RAM and there is no
     * framebuffer to re-read); true = buffered (FastEPD holds a full frame and blits).
     * The core needs this to decide whether a partial update can be composed at all. */
    bool        needs_framebuffer;
    /* Whether 0x76 / PIPE-partial is available on THIS panel, as opposed to on this target. */
    bool        supports_partial;
} od_panel_caps_t;

/* The backend table. Selected ONCE in od_hal_panel_init() from panel_ic_type and
 * display_technology -- exactly the predicate fastepd_driver_used() already computes -- and
 * never consulted by the core, which only ever calls the od_hal_panel_* functions below.
 *
 * Per-controller quirks (EP397 Y-decrement, EP426 X-decrement, the E1004 dual-CS split) live
 * behind this table and must never leak into shared/core. */
struct od_panel_ops {
    const char *name;
    /* "Is this panel mine?" Each backend owns its OWN panel list, rather than the selector
     * encoding both. That is not just tidiness: the FastEPD panel IDs live in
     * src/protocol_pending.h, which is C++ (it guards itself with a static_assert), and the
     * selector is plain C because shared/hal must be. Asking the backend keeps the C++ where
     * it already is and stops the selector growing a second copy of a list that changes
     * whenever a panel family is added. */
    bool (*claims)(uint16_t panel_ic_type, uint8_t display_technology);
    int  (*init)(const struct DisplayConfig *d, const struct SystemConfig *sys,
                 uint16_t panel_ic_type, od_panel_caps_t *caps_out);
    int  (*begin)(void);
    int  (*begin_region)(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    int  (*write)(const uint8_t *bytes, uint32_t len);
    int  (*refresh_start)(od_refresh_t mode);
    bool (*refresh_busy)(void);
    void (*sleep)(void);
    void (*abort)(void);
    void (*mark_deinitialized)(void);
};

/* <0 on failure. caps_out is filled only on success; the core must not read it otherwise. */
int  od_hal_panel_init(const struct DisplayConfig *d, const struct SystemConfig *sys,
                       uint16_t panel_ic_type, od_panel_caps_t *caps_out);

/* Open a full-frame write session using the configured direct stream's initial plane. */
int  od_hal_panel_begin(void);

/* Open a partial-region session. Returns OD_PANEL_ENOTSUP when caps.supports_partial is false,
 * which the core is expected to have checked -- the check here is a backstop, not the gate. */
int  od_hal_panel_begin_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* Sequential sink. Bytes are appended to the open session; for a full-frame session the backend
 * tracks direct-stream part boundaries from the configured color geometry. */
int  od_hal_panel_write(const uint8_t *bytes, uint32_t len);

/* Returns as soon as the controller has been told to refresh -- correction 3. Poll
 * od_hal_panel_refresh_busy() to completion; do NOT sleep the loop task waiting. */
int  od_hal_panel_refresh_start(od_refresh_t mode);
bool od_hal_panel_refresh_busy(void);

void od_hal_panel_sleep(void);

/* Drop any open session without refreshing. Safe with no session open; used by the abort path,
 * which must reach a known state without knowing what the panel was mid-way through. */
void od_hal_panel_abort(void);

/* The rail was cut behind our back, so the next session must run a full controller init rather
 * than a wake(). Separate from sleep() because the two are not the same event: sleep is
 * something we asked for, this is something that happened to us. */
void od_hal_panel_mark_deinitialized(void);

/* Which backend won selection, for logging. NULL before a successful init. */
const char *od_hal_panel_backend_name(void);

#ifdef __cplusplus
}
#endif
