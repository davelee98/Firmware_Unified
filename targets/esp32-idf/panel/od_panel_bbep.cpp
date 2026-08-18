/* od_panel_bbep -- the bb_epaper backend of od_hal_panel.
 *
 * STREAMING BACKEND, and that is the defining difference from FastEPD: bb_epaper writes
 * straight into controller RAM and there is no framebuffer to read back. caps.needs_framebuffer
 * is false, and the core must not assume it can compose a partial update from what is already
 * on the panel. Correction 2 in docs/SHARED_API_DESIGN.md exists to make that askable.
 *
 * FULL-FRAME PLANE ORDER follows od_color geometry. The separate partial-region contract
 * normalizes old/new planes, but begin_region() is not implemented here yet; the live partial
 * path remains in display_service.cpp and performs that vendor translation itself.
 *
 * SCOPE, stated plainly: this covers the full-frame streaming session, refresh, busy and sleep.
 * begin_region() is NOT implemented here and returns OD_PANEL_ENOTSUP -- the partial-window
 * path (addr-window setup, the EP397 Y-decrement and EP426 X-decrement quirks, the E1004
 * dual-CS split) still lives in display_service.cpp and is moved by the repoint, not copied
 * here. A second copy of that logic running alongside the original is exactly the divergence
 * MIGRATION.md's "do not batch subsystem swaps" is written to prevent.
 */
#include "od_hal_panel.h"


#include "bb_epaper.h"
#include "od_color.h"
#include "od_bbep_stream.h"
#include "opendisplay_structs.h"
#include "protocol_pending.h"
#include "od_log.h"
#include "od_hal_time.h"

/* The one display object, defined in main.h with the rest of this firmware's globals. */
extern BBEPDISP bbep;

/* Declared in display_service.cpp; the panel-type mapping stays there because it is a wire
 * contract (OD_PANEL_IC_* -> bb_epaper enum), not a driver detail. */
int mapPanelIcToBbepType(uint16_t panel_ic_type);

/* bb_epaper's C entry points. Declared here for the same reason display_service.cpp declares
 * them: they are defined in panel/od_bbep.cpp's translation unit (bb_ep.inl), and bb_epaper.h
 * declares only some of them. */
void bbepStartWrite(BBEPDISP *pBBEP, int iPlane);
void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen);
int  bbepRefresh(BBEPDISP *pBBEP, int iMode);
bool bbepIsBusy(BBEPDISP *pBBEP);
void bbepSleep(BBEPDISP *pBBEP, int deepSleep);

/* The fallback backend: bb_epaper drives every panel no other backend claims, so this is
 * unconditionally true and the selector relies on it being last in the list. */
static bool bbep_ops_claims(uint16_t panel_ic_type, uint8_t display_technology)
{
    (void)panel_ic_type; (void)display_technology;
    return true;
}

static bool s_session_open = false;
static bool s_refreshing   = false;
static bool s_init_ok      = false;
static uint8_t  s_plane_count  = 1;
static od_color_plane_t s_initial_plane = OD_COLOR_PLANE_0;
static uint32_t s_plane_bytes  = 0;   /* bytes per plane; the plane switch point */
static uint32_t s_written      = 0;

static int bbep_ops_init(const struct DisplayConfig *d, const struct SystemConfig *sys,
                         uint16_t panel_ic_type, od_panel_caps_t *caps_out)
{
    (void)sys;
    if (d == NULL || caps_out == NULL) {
        return OD_PANEL_EINVAL;
    }
    const int type = mapPanelIcToBbepType(panel_ic_type);
    if (type == EP_PANEL_UNDEFINED) {
        od_log_error("panel: bb_epaper has no entry for panel_ic 0x%04X", (unsigned)panel_ic_type);
        return OD_PANEL_ERR;
    }
    od_color_geometry_t geometry;
    if (od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                                 &geometry) != OD_COLOR_OK ||
        geometry.layout == OD_COLOR_LAYOUT_SPLIT_HALVES) {
        return OD_PANEL_ENOTSUP;
    }
    /* Deliberately NOT calling bbepInitIO() here. Bring-up is sequenced with the panel rail
     * (pwrmgm) and the reset pulse by display_service.cpp's session code, which the repoint
     * moves; initialising the bus from underneath it would give the SPI device two owners --
     * the same defect class step 13 removed. */
    caps_out->width  = d->pixel_width;
    caps_out->height = d->pixel_height;
    if (geometry.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES) {
        if (d->color_scheme == OD_COLOR_SCHEME_BWR) caps_out->fmt = OD_PIX_BWR;
        else if (d->color_scheme == OD_COLOR_SCHEME_BWY) caps_out->fmt = OD_PIX_BWY;
        else caps_out->fmt = OD_PIX_4GRAY;
    } else {
        switch (geometry.bits_per_pixel) {
            case 4:  caps_out->fmt = OD_PIX_4GRAY; break;
            case 2:  caps_out->fmt = OD_PIX_2BPP;  break;
            default: caps_out->fmt = OD_PIX_1BPP;  break;
        }
    }

    /* Stream parts come from the geometry, not the display-format label. Packed 4-bpp
     * streams are one part even though OD_PIX_4GRAY is the closest existing HAL label. */
    caps_out->plane_count = geometry.part_count;
    caps_out->needs_framebuffer = false;   /* streaming sink -- see the header comment */
    /* BOTH halves must agree. partial_update_support is what the CONFIG declares over the
     * wire; pInitPart is whether this panel actually has a partial init sequence compiled in.
     * A config claiming partial on a panel bb_epaper cannot partial-refresh would have the
     * core offer 0x76 and the panel ignore it -- a silent failure, which is the class this
     * whole caps query exists to remove. */
    caps_out->supports_partial  = geometry.partial_supported &&
                                  (d->partial_update_support != OD_PARTIAL_UPDATE_NONE) &&
                                  (bbep.pInitPart != NULL);

    s_plane_count = caps_out->plane_count;
    s_initial_plane = geometry.initial_plane;
    s_plane_bytes = geometry.part_bytes[0];
    s_init_ok = true;
    return OD_PANEL_OK;
}

static int bbep_ops_begin(void)
{
    if (!s_init_ok) return OD_PANEL_ERR;
    bbepStartWrite(&bbep, s_initial_plane == OD_COLOR_PLANE_0 ? PLANE_0 : PLANE_1);
    s_written      = 0;
    s_session_open = true;
    return OD_PANEL_OK;
}

static int bbep_ops_begin_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)x; (void)y; (void)w; (void)h;
    /* See the scope note at the top of this file. Not "unsupported by bb_epaper" -- unsupported
     * BY THIS BACKEND until the repoint moves the windowing code, and the distinction matters
     * because caps.supports_partial can still be true. */
    return OD_PANEL_ENOTSUP;
}

static int bbep_ops_write(const uint8_t *bytes, uint32_t len)
{
    if (!s_session_open) return OD_PANEL_EINVAL;
    if (bytes == NULL || len == 0) return OD_PANEL_EINVAL;

    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off;
        /* Switch planes exactly at the boundary, never mid-chunk: bb_epaper's plane switch is a
         * controller command, so a chunk that straddles it must be split rather than sent to
         * whichever plane happens to be selected. */
        if (s_plane_count > 1 && s_written < s_plane_bytes &&
            s_written + chunk > s_plane_bytes) {
            chunk = s_plane_bytes - s_written;
        }
        bbepWriteData(&bbep, (uint8_t *)(bytes + off), (int)chunk);
        s_written += chunk;
        off       += chunk;
        if (s_plane_count > 1 && s_written == s_plane_bytes) {
            bbepStartWrite(&bbep, s_initial_plane == OD_COLOR_PLANE_0 ? PLANE_1 : PLANE_0);
        }
    }
    return OD_PANEL_OK;
}

static int bbep_ops_refresh_start(od_refresh_t mode)
{
    if (!s_init_ok) return OD_PANEL_ERR;
    /* bb_epaper's REFRESH_* ints are not od_refresh_t's; map rather than cast. */
    int m = REFRESH_FULL;
    if (mode == OD_REFRESH_PARTIAL) m = REFRESH_PARTIAL;
    else if (mode == OD_REFRESH_FAST) m = REFRESH_FAST;

    /* NOTE: bbepRefresh() issues the refresh command and returns; it does NOT wait. That is
     * what correction 3 wants, and it is why this backend satisfies the non-blocking contract
     * where the FastEPD one currently cannot. */
    bbepRefresh(&bbep, m);
    s_session_open = false;
    s_refreshing   = true;
    return OD_PANEL_OK;
}

static bool bbep_ops_refresh_busy(void)
{
    if (!s_refreshing) return false;
    if (bbepIsBusy(&bbep)) return true;
    s_refreshing = false;
    return false;
}

static void bbep_ops_sleep(void)
{
    bbepSleep(&bbep, 1);
    s_refreshing   = false;
    s_session_open = false;
}

static void bbep_ops_abort(void)
{
    /* Close any open data stream so the controller is not left mid-transfer with CS asserted.
     * Idempotent -- od_bbep_stream_end() is safe without a matching begin, which matters
     * because the abort path can run twice. */
    od_bbep_stream_end();
    s_session_open = false;
    s_refreshing   = false;
}

static void bbep_ops_mark_deinitialized(void)
{
    bbep.is_awake  = 0;
    s_session_open = false;
    s_refreshing   = false;
    s_init_ok      = false;
}

extern "C" const struct od_panel_ops od_panel_ops_bbep = {
    "bb_epaper",
    bbep_ops_claims,
    bbep_ops_init,
    bbep_ops_begin,
    bbep_ops_begin_region,
    bbep_ops_write,
    bbep_ops_refresh_start,
    bbep_ops_refresh_busy,
    bbep_ops_sleep,
    bbep_ops_abort,
    bbep_ops_mark_deinitialized,
};
