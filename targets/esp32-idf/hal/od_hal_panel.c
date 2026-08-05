/* od_hal_panel -- backend selection and dispatch.
 *
 * The ONLY place a backend is chosen, and the only place the ops table is dereferenced. The
 * core calls od_hal_panel_* and never sees struct od_panel_ops at all; that is what keeps the
 * one permitted vtable from becoming a pattern (CLAUDE.md, shared API consequence 1).
 *
 * Selection asks each backend whether the panel is its own (ops->claims), in order, with
 * bb_epaper last as the fallback. The predicate is equivalent to fastepd_driver_used() in
 * display_service.cpp, but is NOT a call to it: that function reads globalConfig directly,
 * and this HAL takes its config by argument so it can be promoted to shared/hal without
 * dragging a global along.
 */
#include "od_hal_panel.h"
/* opendisplay_structs.h, NOT src/structs.h: DisplayConfig is a WIRE type and lives in the
 * protocol header, which is pure C. src/structs.h is C++ (default arguments, string
 * constants) and including it here would force this file to be C++ for no reason -- and
 * shared/hal is plain C by rule. protocol_pending.h carries the two Inkplate panel IDs that
 * have not reached the canonical header yet; it is #defines only. */
#include "opendisplay_structs.h"
#include "od_log.h"

#include <stddef.h>

extern const struct od_panel_ops od_panel_ops_bbep;
#if defined(TARGET_ESP32) && defined(OPENDISPLAY_FASTEPD)
extern const struct od_panel_ops od_panel_ops_fastepd;
#endif

static const struct od_panel_ops *s_ops = NULL;

static const struct od_panel_ops *select_backend(const struct DisplayConfig *d,
                                                 uint16_t panel_ic_type)
{
    /* Ordered, and the order is the policy: a specialised backend gets first refusal and
     * bb_epaper is the fallback that claims everything else. Adding a backend means adding it
     * to this list ABOVE bb_epaper, not editing a predicate. */
#if defined(TARGET_ESP32) && defined(OPENDISPLAY_FASTEPD)
    if (od_panel_ops_fastepd.claims(panel_ic_type, d->display_technology)) {
        return &od_panel_ops_fastepd;
    }
#else
    (void)panel_ic_type;
    (void)d;
#endif
    return &od_panel_ops_bbep;
}

int od_hal_panel_init(const struct DisplayConfig *d, const struct SystemConfig *sys,
                      uint16_t panel_ic_type, od_panel_caps_t *caps_out)
{
    if (d == NULL || caps_out == NULL) {
        return OD_PANEL_EINVAL;
    }
    s_ops = select_backend(d, panel_ic_type);
    const int rc = s_ops->init(d, sys, panel_ic_type, caps_out);
    if (rc < 0) {
        /* Leave s_ops set: the core may still call abort()/mark_deinitialized() on the way
         * down, and a NULL table there would turn a failed init into a crash. */
        od_log_error("panel: %s backend init failed (%d)", s_ops->name, rc);
        return rc;
    }
    od_log_info("panel: %s backend, %ux%u, %u plane(s), %s, partial=%s",
                s_ops->name, (unsigned)caps_out->width, (unsigned)caps_out->height,
                (unsigned)caps_out->plane_count,
                caps_out->needs_framebuffer ? "buffered" : "streaming",
                caps_out->supports_partial ? "yes" : "no");
    return rc;
}

/* Every entry point below tolerates being called before a successful init. The panel is the one
 * subsystem whose teardown runs from paths that do not know whether bring-up got that far --
 * the session abort, the deep-sleep gate and the reboot teardown all reach it. */
int od_hal_panel_begin(void)
{
    return s_ops ? s_ops->begin() : OD_PANEL_EINVAL;
}

int od_hal_panel_begin_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    return s_ops ? s_ops->begin_region(x, y, w, h) : OD_PANEL_EINVAL;
}

int od_hal_panel_write(const uint8_t *bytes, uint32_t len)
{
    return s_ops ? s_ops->write(bytes, len) : OD_PANEL_EINVAL;
}

int od_hal_panel_refresh_start(od_refresh_t mode)
{
    return s_ops ? s_ops->refresh_start(mode) : OD_PANEL_EINVAL;
}

bool od_hal_panel_refresh_busy(void)
{
    /* No backend means nothing is refreshing. Reporting "busy" here would hang the deep-sleep
     * gate, which polls this to decide whether the panel may be powered down. */
    return s_ops ? s_ops->refresh_busy() : false;
}

void od_hal_panel_sleep(void)
{
    if (s_ops) s_ops->sleep();
}

void od_hal_panel_abort(void)
{
    if (s_ops) s_ops->abort();
}

void od_hal_panel_mark_deinitialized(void)
{
    if (s_ops) s_ops->mark_deinitialized();
}

const char *od_hal_panel_backend_name(void)
{
    return s_ops ? s_ops->name : NULL;
}
