#include "opendisplay_display.h"

#include "od_color.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
#include "opendisplay_epd_map.h"
#include "opendisplay_protocol.h"
#include "od_runtime_types.h"
#include "opendisplay_touch.h"
#include "od_board.h"
#include "boot_screen.h"
#include "od_gpio.h"
#include "od_zephyr_compat.h"
#include "od_watchdog_app.h"
#include "bb_epaper.h"
#include "od_bbep_zephyr.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "od_zlib_pump.h"
}

void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);

#define OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE 256u

/* Raw BBEPDISP, not the vendored BBEPAPER C++ class -- see panel/od_bbep_zephyr.h. The class is
 * not compiled on this target, exactly as on esp32-idf. */
static BBEPDISP s_epd;
static bool s_active;
static uint32_t s_total_bytes;
static uint32_t s_written_bytes;
static uint32_t s_dw_chunk_n;
static uint8_t s_dw_log_pct;
static uint8_t s_dw_trailing_ignores;
static uint32_t s_dw_init_t0;
static uint8_t s_color_scheme;
static uint32_t s_plane_size;
static bool s_plane2_started;
static bool s_boot_applied;
static bool s_dw_compressed;
static uint32_t s_dw_decompressed_total;
static uint8_t s_decompression_chunk[OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE];
static uint32_t s_displayed_etag;
static bool s_epd_rail_conditioned;

static const uint8_t PARTIAL_FLAG_COMPRESSED = 0x01u;
static const uint8_t PARTIAL_ALLOWED_FLAGS = PARTIAL_FLAG_COMPRESSED;

struct PartialStreamContext {
  bool active;
  bool compressed;
  uint8_t flags;
  uint32_t new_etag;
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
  uint32_t expected_stream_size;
  uint32_t plane_size;
  uint32_t bytes_received;
  uint32_t bytes_written;
  uint8_t current_plane;
};

static PartialStreamContext s_partial;
static bool s_partial_panel_up;

static void dw_init_mark(const char *tag)
{
  uint32_t now = od_uptime_get_32();
  od_log_info("dw init %-26s %lu ms", tag, (unsigned long)(now - s_dw_init_t0));
}

static const struct DisplayConfig *display_cfg(void)
{
  const struct od_config *cfg = opendisplay_get_global_config();
  if (cfg == nullptr || cfg->display_count == 0u) {
    return nullptr;
  }
  return &cfg->displays[0];
}

static void display_park_signal_pin(uint8_t pin_cfg)
{
  od_gpio_park(pin_cfg);
}

void opendisplay_display_park_pins(void)
{
  const struct DisplayConfig *d = display_cfg();

  if (d == nullptr) {
    return;
  }
  display_park_signal_pin(d->cs_pin);
  display_park_signal_pin(d->data_pin);
  display_park_signal_pin(d->clk_pin);
  display_park_signal_pin(d->dc_pin);
  display_park_signal_pin(d->reset_pin);
  display_park_signal_pin(d->busy_pin);
}

/*
 * PANEL RAIL BRING-UP. Ported from the Arduino reference's pwrmgm(true)
 * (Firmware/src/main.cpp:1341-1382), which is the behaviour the deployed configs were
 * validated against.
 *
 * THE RAIL NEEDS ~900 ms BEFORE THE PANEL WILL TALK, and this function used to return the
 * instant the enable pin went high. The whole init then ran against an unpowered controller:
 *
 *     dw init after cfg           0 ms
 *     dw init after setPanelType  2 ms
 *     dw init after initIO        8 ms     <- RST already toggling here
 *     dw init after wake          60 ms    <- BUSY sampled here
 *
 * At 8 ms the reference has not yet finished the first of its two delays. bbepIsBusy() reads
 * an unpowered BUSY line as idle, bbepWakeUp()'s wait returns immediately, pInitFull and all
 * 48000 bytes are clocked into a dead controller, and the only symptom is the 60 s
 * "BUSY NEVER ASSERTED" at refresh time. Nothing upstream of that reports an error.
 *
 * The reference's two delays are separate and both are needed: 800 ms for the rail itself,
 * then the signal lines are pre-driven to their idle levels and 100 ms lets the panel see
 * them settle before RST is toggled.
 */
#define OD_PANEL_RAIL_SETTLE_MS 800u
#define OD_PANEL_PIN_SETTLE_MS  100u

static bool display_power_set(bool on)
{
  const struct od_config *cfg = opendisplay_get_global_config();
  const struct DisplayConfig *d = display_cfg();
  uint8_t p;

  /* Never bring the panel rail up in watchdog safe mode. Powering DOWN is still allowed, so a
   * rail left on by a pre-safe-mode boot can still be shut off. This is the same gate the
   * reference puts at the top of pwrmgm() (esp32-idf main.cpp), and display_power_set is this
   * target's pwrmgm -- one refusal point covers boot refresh and every pushed image alike. */
  if (on && od_watchdog_app_safe_mode()) {
    od_log_warn("panel: power-up refused - watchdog safe mode");
    return false;
  }
  if (cfg == nullptr) {
    od_log_error("panel: global config unavailable");
    return false;
  }
  p = cfg->system_config.pwr_pin;
  /*
   * 0xFF means the pin is NOT PRESENT: the rail is permanently powered and cannot be turned
   * off. There is then no pin to drive, no rail settle to wait for, and nothing to park --
   * the panel stays live either way, so leaving its signal lines driven at their idle levels
   * is safer than floating them into a powered controller.
   *
   * What must never happen is substituting a fallback pin number; driving a guessed pin is
   * the unsafe case, not this configuration. 0xFF is the contract's documented default
   * (opendisplay_structs.h: "primary power-management pin; 0xFF = not present"), so refusing
   * it outright disabled the display on every board wired without a rail switch.
   * Matches pwrmgm() (esp32-idf main.cpp:1382 on, :1422 off).
   */
  const bool has_pwr_pin = (p != 0xFFu);

  if (!on) {
    od_watchdog_app_phase(OD_WDT_PHASE_FORCE_OFF);
    if (has_pwr_pin) {
      opendisplay_display_park_pins();
      od_gpio_configure_output(p, false);
    }
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE_OFF);
    return true;
  }

  /* Rail bring-up, breadcrumbed per sub-step. A 2026-08-03 freeze on the reference reset ~120 s
   * into bring-up without ever reaching the init sequence, and a single ACQUIRE_COLD stamp could
   * not say which step it died in. There is no PWRMGM_PMIC stamp here because no PMIC sits on
   * this path -- the nPM1300 is read as a sensor, not brought up to power the panel. */
  od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_RAIL);

  /* Boost-select conditioning is a panel-interface setting, not a rail switch: the panel
   * needs it whether or not this board can cut power. */
  od_board_prepare_epd_rail();

  if (!has_pwr_pin) {
    od_log_warn("panel: pwr_pin not present; rail is permanently powered");
  } else {
    od_gpio_configure_output(p, true);

    /* The deployed nRF52840 board requires the donor firmware's one-time cold
     * rail conditioning: BS low, on 50 ms, off 50 ms, BS low, on. nRF54 board
     * implementations return false and retain their existing single power-on. */
    if (!s_epd_rail_conditioned && od_board_epd_requires_cold_cycle()) {
      od_msleep(50);
      od_gpio_configure_output(p, false);
      od_msleep(50);
      od_board_prepare_epd_rail();
      od_gpio_configure_output(p, true);
    }
    s_epd_rail_conditioned = true;
    od_log_debug("panel: rail on (pwr_pin=%u), settling %u ms", (unsigned)p,
                 (unsigned)OD_PANEL_RAIL_SETTLE_MS);
    od_msleep(OD_PANEL_RAIL_SETTLE_MS);
  }

  /* Pre-drive the signal lines to their idle levels, exactly as pwrmgm() does, so the panel
   * comes out of its own power-on reset seeing a quiet bus rather than floating inputs. */
  od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_PINS);
  if (d != nullptr) {
    od_gpio_configure_output(d->reset_pin, true);   /* RST idle HIGH */
    od_gpio_configure_output(d->cs_pin, true);      /* CS  idle HIGH */
    od_gpio_configure_output(d->dc_pin, false);
    od_gpio_configure_output(d->clk_pin, false);
    od_gpio_configure_output(d->data_pin, false);
    od_gpio_configure_input(d->busy_pin, false, false);
    od_msleep(OD_PANEL_PIN_SETTLE_MS);
  }
  return true;
}

void opendisplay_display_power_off(void)
{
  display_power_set(false);
}

static bool wait_for_refresh(uint32_t timeout_ms)
{
  uint32_t elapsed = 0;
  bool saw_busy = false;

  od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);

  /*
   * READ THE EXIT CONDITION CAREFULLY: this returns true only after it has seen BUSY go
   * ASSERTED and then RELEASED. If BUSY never asserts at all -- panel not actually driven,
   * or the BUSY line mis-decoded, or the wrong pull for the chip type -- the loop runs the
   * FULL timeout in silence and then returns false. At the 60 s the caller passes, that is
   * a minute of apparent hang with no output, during which the host gives up and its next
   * attempt is refused with "Pipe write already in progress".
   *
   * The od_log_debug() lines exist so that case is distinguishable from a panel that is simply
   * slow: "busy asserted" appearing at all is the difference between "the panel is talking
   * to us" and "we are bit-banging into the void".
   */
  while (elapsed < timeout_ms) {
    bool busy = bbepIsBusy(&s_epd);
    if (busy) {
      if (!saw_busy) {
        od_log_debug("refresh: busy asserted after %u ms", (unsigned)elapsed);
      }
      saw_busy = true;
    } else if (saw_busy) {
      od_log_debug("refresh: busy released after %u ms", (unsigned)elapsed);
      return true;
    }
    od_msleep(50);
    elapsed += 50;
  }
  if (!saw_busy) {
    od_log_debug("refresh: BUSY NEVER ASSERTED in %u ms -- panel is not responding; "
                 "check the BUSY pin decode and the panel power rail",
                 (unsigned)timeout_ms);
  } else {
    od_log_debug("refresh: busy asserted but still held at %u ms timeout",
           (unsigned)timeout_ms);
  }
  return saw_busy && !bbepIsBusy(&s_epd);
}

static uint32_t parse_be_u32(const uint8_t *data)
{
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8)
         | (uint32_t)data[3];
}

static uint32_t mono_plane_bytes(uint16_t width, uint16_t height)
{
  od_color_geometry_t geometry;
  return od_color_direct_geometry(OD_COLOR_SCHEME_MONO, width, height, &geometry) == OD_COLOR_OK
      ? geometry.part_bytes[0] : 0u;
}

static void partial_cleanup(void)
{
  if (!s_partial.active) {
    return;
  }
  if (s_partial_panel_up) {
    bbepSleep(&s_epd, DEEP_SLEEP);
    display_power_set(false);
    s_partial_panel_up = false;
  }
  memset(&s_partial, 0, sizeof(s_partial));
}

static bool panel_skips_bbep_set_addr_window(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY || pBBEP->type == EP426_800x480
         || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_uses_pixel_ram_x(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY || pBBEP->type == EP426_800x480
         || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_uses_ep397_y_decrement(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY;
}

static bool panel_uses_ep426_x_decrement(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP426_800x480 || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_skips_reinit_on_partial_refresh(const BBEPDISP *pBBEP)
{
  return panel_uses_ep397_y_decrement(pBBEP) || panel_uses_ep426_x_decrement(pBBEP);
}

static void partial_set_ep397_ram_y(BBEPDISP *pBBEP, int ty, int cy)
{
  uint8_t uc[4];
  int yLast = ty + cy - 1;
  int ramYStart = (pBBEP->native_height - 1) - ty;
  int ramYEnd = (pBBEP->native_height - 1) - yLast;

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYPOS);
  uc[0] = (uint8_t)(ramYStart & 0xff);
  uc[1] = (uint8_t)(ramYStart >> 8);
  uc[2] = (uint8_t)(ramYEnd & 0xff);
  uc[3] = (uint8_t)(ramYEnd >> 8);
  bbepWriteData(&s_epd, uc, 4);

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYCOUNT);
  uc[0] = (uint8_t)(ramYStart & 0xff);
  uc[1] = (uint8_t)(ramYStart >> 8);
  bbepWriteData(&s_epd, uc, 2);
}

static void partial_set_ep426_ram_y(BBEPDISP *pBBEP, int ty, int cy)
{
  uint8_t uc[4];
  int yLast = ty + cy - 1;

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYPOS);
  uc[0] = (uint8_t)ty;
  uc[1] = (uint8_t)(ty >> 8);
  uc[2] = (uint8_t)yLast;
  uc[3] = (uint8_t)(yLast >> 8);
  bbepWriteData(&s_epd, uc, 4);

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYCOUNT);
  uc[0] = (uint8_t)ty;
  uc[1] = (uint8_t)(ty >> 8);
  bbepWriteData(&s_epd, uc, 2);
  (void)pBBEP;
}

static void partial_set_pixel_ram_x(BBEPDISP *pBBEP, int x, int cx)
{
  uint8_t uc[4];
  int px0 = x;
  int px1 = x + cx - 1;
  if (panel_uses_ep426_x_decrement(pBBEP)) {
    px0 = (pBBEP->native_width - 1) - x;
    px1 = (pBBEP->native_width - 1) - (x + cx - 1);
  }

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMXPOS);
  uc[0] = (uint8_t)(px0 & 0xff);
  uc[1] = (uint8_t)((px0 >> 8) & 0xff);
  uc[2] = (uint8_t)(px1 & 0xff);
  uc[3] = (uint8_t)(px1 >> 8);
  bbepWriteData(&s_epd, uc, 4);

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMXCOUNT);
  uc[0] = (uint8_t)(px0 & 0xff);
  uc[1] = (uint8_t)(px0 >> 8);
  bbepWriteData(&s_epd, uc, 2);
}

static void partial_set_addr_window(BBEPDISP *pBBEP, int x, int y, int cx, int cy)
{
  if (!panel_skips_bbep_set_addr_window(pBBEP)) {
    bbepSetAddrWindow(&s_epd, x, y, cx, cy);
    return;
  }

  uint8_t uc[4];
  int ty = y;
  cx = (cx + 7) & 0xfff8;

  if (panel_uses_pixel_ram_x(pBBEP)) {
    partial_set_pixel_ram_x(pBBEP, x, cx);
  } else {
    int tx = x / 8;
    bbepWriteCmd(&s_epd, SSD1608_SET_RAMXPOS);
    uc[0] = (uint8_t)tx;
    uc[1] = (uint8_t)(tx + ((cx - 1) >> 3));
    bbepWriteData(&s_epd, uc, 2);
    bbepCMD2(&s_epd, SSD1608_SET_RAMXCOUNT, (uint8_t)tx);
  }

  if (panel_uses_ep426_x_decrement(pBBEP)) {
    partial_set_ep426_ram_y(pBBEP, ty, cy);
  } else if (panel_uses_ep397_y_decrement(pBBEP)) {
    partial_set_ep397_ram_y(pBBEP, ty, cy);
  } else {
    bbepWriteCmd(&s_epd, SSD1608_SET_RAMYPOS);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    uc[2] = (uint8_t)(ty + cy - 1);
    uc[3] = (uint8_t)((ty + cy - 1) >> 8);
    bbepWriteData(&s_epd, uc, 4);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    bbepWriteCmd(&s_epd, SSD1608_SET_RAMYCOUNT);
    bbepWriteData(&s_epd, uc, 2);
  }
  bbepWaitBusy(&s_epd);
}

static bool partial_write_stream_bytes(uint8_t *data, uint32_t len)
{
  /* Per-frame path; see dw_stream_raw_bytes(). */
  od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
  uint32_t offset = 0;
  while (offset < len) {
    if (s_partial.bytes_written >= s_partial.expected_stream_size) {
      return false;
    }

    uint8_t targetPlane = s_partial.bytes_written < s_partial.plane_size ? PLANE_1 : PLANE_0;
    if (s_partial.current_plane != targetPlane) {
      if (targetPlane == PLANE_0 && s_partial.bytes_written != s_partial.plane_size) {
        return false;
      }
      partial_set_addr_window(&s_epd, s_partial.x, s_partial.y, s_partial.width, s_partial.height);
      bbepStartWrite(&s_epd, targetPlane);
      s_partial.current_plane = targetPlane;
    }

    uint32_t planeEnd =
      targetPlane == PLANE_1 ? s_partial.plane_size : s_partial.expected_stream_size;
    uint32_t chunk = planeEnd - s_partial.bytes_written;
    if (chunk > len - offset) {
      chunk = len - offset;
    }
    bbepWriteData(&s_epd, data + offset, (int)chunk);
    s_partial.bytes_written += chunk;
    offset += chunk;
  }
  return true;
}

static bool partial_zlib_sink(void *, od_mut_span_t bytes)
{
  return partial_write_stream_bytes(bytes.p, (uint32_t)bytes.n);
}

static bool zlib_stream_to_partial_write(const uint8_t *data, uint32_t len, bool final)
{
  od_mut_span_t scratch = od_mut_span_make(s_decompression_chunk,
                                           OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE);
  od_zlib_pump_status_t status = od_zlib_pump_push(
    od_span_make(data, len), final, scratch, partial_zlib_sink, nullptr);
  if (status == OD_ZLIB_PUMP_ERROR) {
    od_log_info("partial zlib error: %s", od_zlib_pump_error());
    return false;
  }
  return !final || (status == OD_ZLIB_PUMP_DONE
                    && s_partial.bytes_written == s_partial.expected_stream_size);
}

static bool partial_consume_bytes(uint8_t *data, uint32_t len)
{
  if (s_partial.compressed) {
    if (len > UINT32_MAX - s_partial.bytes_received) {
      return false;
    }
  } else if (s_partial.bytes_received > s_partial.expected_stream_size
             || len > s_partial.expected_stream_size - s_partial.bytes_received) {
    return false;
  }
  s_partial.bytes_received += len;
  if (s_partial.compressed) {
    return zlib_stream_to_partial_write(data, len, false);
  }
  return partial_write_stream_bytes(data, len);
}

static bool partial_trigger_refresh(int refresh_mode)
{
  if (refresh_mode < 0 || refresh_mode > 3) {
    refresh_mode = REFRESH_PARTIAL;
  }
  if (panel_skips_reinit_on_partial_refresh(&s_epd)) {
    if (panel_uses_ep397_y_decrement(&s_epd)) {
      static const uint8_t u8cmdz3[4] = { 0xf7, 0xd7, 0xff, 0 };
      bbepCMD2(&s_epd, SSD1608_DISP_CTRL2, u8cmdz3[refresh_mode]);
    } else {
      static const uint8_t u8cmd[4] = { 0xf7, 0xc7, 0xff, 0xc0 };
      bbepCMD2(&s_epd, SSD1608_DISP_CTRL2, u8cmd[refresh_mode]);
    }
    bbepWriteCmd(&s_epd, SSD1608_MASTER_ACTIVATE);
    return wait_for_refresh(60000u);
  }
  (void)bbepRefresh(&s_epd, refresh_mode);
  return wait_for_refresh(60000u);
}

static bool partial_prepare_panel_ram(void)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;

  if (d == nullptr) {
    return false;
  }
  panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    return false;
  }

  if (!display_power_set(true)) {
    return false;
  }
  memset(&s_epd, 0, sizeof(s_epd));
  if (bbepSetPanelType(&s_epd, panel) != BBEP_SUCCESS) {
    display_power_set(false);
    return false;
  }
  if ((s_epd.iFlags & BBEP_SPLIT_BUFFER) != 0u) {
    od_log_error("split-panel transport is not hardware-qualified on this target");
    display_power_set(false);
    return false;
  }
  bbepSetRotation(&s_epd, (int)d->rotation * 90);
  bbepInitIO(&s_epd, d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin, 0);
  od_bbep_wake(&s_epd);
  {
    const uint8_t *init_seq = s_epd.pInitPart ? s_epd.pInitPart : s_epd.pInitFull;
    bbepSendCMDSequence(&s_epd, init_seq);
  }
  bbepFill(&s_epd, BBEP_WHITE, PLANE_1);
  bbepFill(&s_epd, BBEP_WHITE, PLANE_0);
  s_partial_panel_up = true;
  return true;
}

static bool partial_write_to_panel(int refresh_mode)
{
  if (s_partial.bytes_written != s_partial.expected_stream_size) {
    return false;
  }
  od_msleep(20);
  bool ok = partial_trigger_refresh(refresh_mode);
  bbepSleep(&s_epd, DEEP_SLEEP);
  display_power_set(false);
  s_partial_panel_up = false;
  return ok;
}

extern "C" bool opendisplay_display_partial_active(void)
{
  return s_partial.active;
}

extern "C" bool opendisplay_display_dw_active(void)
{
  return s_active;
}

extern "C" uint32_t opendisplay_display_bytes_written(void)
{
  return s_written_bytes;
}

extern "C" uint32_t opendisplay_display_total_bytes(void)
{
  return s_total_bytes;
}

extern "C" uint32_t opendisplay_display_expected_dw_bytes(void)
{
  const struct DisplayConfig *d = display_cfg();
  od_color_geometry_t geometry;

  if (d == nullptr
      || od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                                  &geometry) != OD_COLOR_OK
      || geometry.layout == OD_COLOR_LAYOUT_SPLIT_HALVES) {
    return 0;
  }
  return geometry.total_bytes;
}

extern "C" uint32_t opendisplay_display_displayed_etag(void)
{
  return s_displayed_etag;
}

extern "C" void opendisplay_display_clear_etag(void)
{
  s_displayed_etag = 0;
}

extern "C" void opendisplay_display_set_partial_new_etag(uint32_t new_etag)
{
  s_partial.new_etag = new_etag;
}

extern "C" uint32_t opendisplay_display_partial_bytes_written(void)
{
  return s_partial.bytes_written;
}

extern "C" uint32_t opendisplay_display_partial_expected(void)
{
  return s_partial.expected_stream_size;
}

extern "C" bool opendisplay_display_partial_compressed(void)
{
  return s_partial.compressed;
}

extern "C" uint32_t opendisplay_display_calc_plane_bytes(uint16_t width, uint16_t height)
{
  return mono_plane_bytes(width, height);
}

extern "C" int opendisplay_display_partial_write_start(const uint8_t *payload, uint16_t payload_len,
                                                       uint8_t *err_code_out)
{
  const struct DisplayConfig *d = display_cfg();

  if (err_code_out != nullptr) {
    *err_code_out = OD_ERR_PARTIAL_STREAM;
  }

  if (s_active) {
    opendisplay_display_abort();
  }
  if (s_partial.active) {
    partial_cleanup();
  }

  if (payload_len < 17u || d == nullptr) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_STREAM;
    }
    return -1;
  }

  uint8_t flags = payload[0];
  uint32_t old_etag = parse_be_u32(payload + 1);
  uint32_t new_etag = parse_be_u32(payload + 5);
  uint16_t rect_x = (uint16_t)(((uint16_t)payload[9] << 8) | payload[10]);
  uint16_t rect_y = (uint16_t)(((uint16_t)payload[11] << 8) | payload[12]);
  uint16_t rect_w = (uint16_t)(((uint16_t)payload[13] << 8) | payload[14]);
  uint16_t rect_h = (uint16_t)(((uint16_t)payload[15] << 8) | payload[16]);

  if ((flags & ~PARTIAL_ALLOWED_FLAGS) != 0u) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_FLAGS;
    }
    return -1;
  }
  if (d->partial_update_support == 0u) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_UNSUPPORTED;
    }
    return -1;
  }
  if (old_etag == 0u || old_etag != s_displayed_etag || new_etag == 0u) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_ETAG_MISMATCH;
    }
    return -1;
  }
  od_color_geometry_t display_geometry;
  if (od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                               &display_geometry) != OD_COLOR_OK
      || !display_geometry.partial_supported) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_UNSUPPORTED;
    }
    return -1;
  }
  if (rect_w == 0u || rect_h == 0u || (uint32_t)rect_x + rect_w > d->pixel_width
      || (uint32_t)rect_y + rect_h > d->pixel_height) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_RECT_OOB;
    }
    return -1;
  }
  if ((rect_x & 7u) != 0u || (rect_w & 7u) != 0u) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_RECT_ALIGN;
    }
    return -1;
  }

  uint32_t plane_bytes = mono_plane_bytes(rect_w, rect_h);
  uint32_t expected_size = plane_bytes * 2u;
  if (expected_size == 0u) {
    return -1;
  }

  memset(&s_partial, 0, sizeof(s_partial));
  s_partial.active = true;
  s_partial.compressed = (flags & PARTIAL_FLAG_COMPRESSED) != 0u;
  s_partial.flags = flags;
  s_partial.new_etag = new_etag;
  s_partial.x = rect_x;
  s_partial.y = rect_y;
  s_partial.width = rect_w;
  s_partial.height = rect_h;
  s_partial.expected_stream_size = expected_size;
  s_partial.plane_size = plane_bytes;
  s_partial.current_plane = 0xFFu;

  if (s_partial.compressed
      && (d->transmission_modes & TRANSMISSION_MODE_STREAMING_DECOMPRESSION) == 0u) {
    partial_cleanup();
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_UNSUPPORTED;
    }
    return -1;
  }

  if (!partial_prepare_panel_ram()) {
    partial_cleanup();
    return -1;
  }

  if (s_partial.compressed) {
    od_zlib_pump_reset(expected_size);
  }

  if (payload_len > 17u) {
    if (!partial_consume_bytes((uint8_t *)(void *)(payload + 17), (uint32_t)payload_len - 17u)) {
      s_displayed_etag = 0;
      partial_cleanup();
      if (err_code_out != nullptr) {
        *err_code_out = OD_ERR_PARTIAL_STREAM;
      }
      return -1;
    }
  }

  od_log_info("partial start etag=%08lX->%08lX rect=%u,%u %ux%u %s",
         (unsigned long)old_etag, (unsigned long)new_etag, (unsigned)rect_x, (unsigned)rect_y,
         (unsigned)rect_w, (unsigned)rect_h, s_partial.compressed ? "zlib" : "raw");
  return 0;
}

extern "C" void opendisplay_display_abort(void)
{
  partial_cleanup();
  if (s_active) {
    bbepSleep(&s_epd, DEEP_SLEEP);
  }
  display_power_set(false);
  s_active = false;
  s_total_bytes = 0;
  s_written_bytes = 0;
  s_dw_chunk_n = 0;
  s_dw_log_pct = 0;
  s_dw_trailing_ignores = 0;
  s_plane_size = 0;
  s_plane2_started = false;
  s_dw_compressed = false;
  s_dw_decompressed_total = 0;
}

static void dw_log_progress(void)
{
  if (s_total_bytes == 0u) {
    return;
  }
  uint8_t pct = (uint8_t)((100u * s_written_bytes) / s_total_bytes);
  if (pct >= s_dw_log_pct + 25u) {
    od_log_info("dw data #%lu %lu/%lu B (%u%%)%s", (unsigned long)s_dw_chunk_n,
           (unsigned long)s_written_bytes, (unsigned long)s_total_bytes, (unsigned)pct,
           s_dw_compressed ? " zlib" : "");
    s_dw_log_pct = (pct / 25u) * 25u;
  }
}

static int dw_stream_raw_bytes(const uint8_t *payload, uint32_t payload_len)
{
  /* Per-frame path. Repeats are filtered inside od_watchdog_breadcrumb(), so a stamp on every
   * chunk costs one comparison once the phase is already STREAM. */
  od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
  uint32_t remaining = (s_written_bytes < s_total_bytes) ? (s_total_bytes - s_written_bytes) : 0u;
  const bool bitplanes = s_plane_size != 0u;
  const uint8_t *p = payload;
  uint32_t left = payload_len;
  const uint32_t written_before = s_written_bytes;

  while (left > 0u && remaining > 0u) {
    uint32_t rem = remaining;
    uint32_t chunk = left;
    if (bitplanes && !s_plane2_started && s_plane_size > 0u) {
      uint32_t to_plane_end = s_plane_size - s_written_bytes;
      if (chunk > to_plane_end) {
        chunk = to_plane_end;
      }
    }
    if (chunk > rem) {
      chunk = rem;
    }
    if (chunk == 0u) {
      break;
    }
    bbepWriteData(&s_epd, (uint8_t *)(void *)p, (int)chunk);
    p += chunk;
    left -= chunk;
    s_written_bytes += chunk;
    remaining -= chunk;
    if (bitplanes && !s_plane2_started && s_plane_size > 0u && s_written_bytes >= s_plane_size) {
      bbepStartWrite(&s_epd, PLANE_1);
      s_plane2_started = true;
    }
  }

  if (s_written_bytes > written_before) {
    dw_log_progress();
  }
  return 0;
}

static bool direct_zlib_sink(void *, od_mut_span_t bytes)
{
  uint32_t before = s_written_bytes;
  if (dw_stream_raw_bytes(bytes.p, (uint32_t)bytes.n) != 0) {
    return false;
  }
  return s_written_bytes - before == (uint32_t)bytes.n
         && s_written_bytes <= s_dw_decompressed_total;
}

static bool zlib_stream_to_direct_write(const uint8_t *data, uint32_t len, bool final)
{
  od_mut_span_t scratch = od_mut_span_make(s_decompression_chunk,
                                           OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE);
  od_zlib_pump_status_t status = od_zlib_pump_push(
    od_span_make(data, len), final, scratch, direct_zlib_sink, nullptr);
  if (status == OD_ZLIB_PUMP_ERROR) {
    od_log_info("zlib error: %s", od_zlib_pump_error());
    return false;
  }
  return !final || (status == OD_ZLIB_PUMP_DONE
                    && s_written_bytes == s_dw_decompressed_total);
}

extern "C" bool opendisplay_display_boot_apply(void)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;
  if (s_boot_applied) {
    return true;
  }
  /* Three consecutive watchdog resets say the panel path is what wedges, so a fourth attempt is
   * another reset. Reported as applied, not as a failure: the caller's failure arm retries, and
   * retrying is exactly what safe mode exists to stop. The device stays up on BLE and DFU, which
   * is the only way a bad config or a bad image can be replaced. */
  if (od_watchdog_app_safe_mode()) {
    od_log_warn("boot display skipped: watchdog safe mode");
    s_boot_applied = true;
    return true;
  }
  if (d == nullptr) {
    od_log_error("boot display: no display config");
    return false;
  }
  if ((d->transmission_modes & TRANSMISSION_MODE_CLEAR_ON_BOOT) != 0u) {
    od_log_info("boot display intentionally skipped by CLEAR_ON_BOOT");
    s_boot_applied = true;
    return true;
  }
  panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    od_log_error("boot display: unsupported panel_ic_type=%u", (unsigned)d->panel_ic_type);
    return false;
  }

  for (unsigned attempt = 1; attempt <= 2; attempt++) {
    bool refresh_ok = false;

    memset(&s_epd, 0, sizeof(s_epd));
    if (!display_power_set(true)) {
      od_log_error("boot display: panel power unavailable (attempt %u)", attempt);
      continue;
    }
    if (bbepSetPanelType(&s_epd, panel) != BBEP_SUCCESS) {
      od_log_error("boot display: setPanelType failed (attempt %u)", attempt);
      display_power_set(false);
      continue;
    }
    if ((s_epd.iFlags & BBEP_SPLIT_BUFFER) != 0u) {
      od_log_error("boot display: split-panel transport is not hardware-qualified");
      display_power_set(false);
      return false;
    }
    bbepSetRotation(&s_epd, (int)d->rotation * 90);
    bbepInitIO(&s_epd, d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin,
               d->clk_pin, 0);
    od_bbep_wake(&s_epd);
    od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
    od_bbep_send_panel_init_full(&s_epd);
    if (!writeBootScreenWithQr(s_epd)) {
      od_log_warn("boot display: renderer failed; transmitting white fallback");
      od_watchdog_app_phase(OD_WDT_PHASE_FILL);
      bbepFill(&s_epd, BBEP_WHITE, PLANE_DUPLICATE);
    }
    od_log_info("boot framebuffer transmitted; starting physical refresh (attempt %u)", attempt);
    if (bbepRefresh(&s_epd, REFRESH_FULL) == BBEP_SUCCESS) {
      refresh_ok = wait_for_refresh(60000u);
    } else {
      od_log_error("boot display: refresh command failed (attempt %u)", attempt);
    }
    bbepSleep(&s_epd, DEEP_SLEEP);
    display_power_set(false);

    if (refresh_ok) {
      s_boot_applied = true;
      od_log_info("boot display refresh complete");
      return true;
    }
    od_log_warn("boot display: physical refresh failed (attempt %u of 2)", attempt);
    if (attempt < 2) {
      od_msleep(100);
    }
  }
  return false;
}

extern "C" int opendisplay_display_direct_write_start(const uint8_t *payload, uint16_t payload_len)
{
  s_dw_init_t0 = od_uptime_get_32();
  od_log_info("dw init begin");

  if (s_partial.active) {
    partial_cleanup();
  }

  const struct DisplayConfig *d = display_cfg();
  if (d == nullptr) {
    od_log_info("dw start err no display cfg");
    return -1;
  }
  dw_init_mark("after cfg");

  od_color_geometry_t geometry;
  od_color_status_t color_status =
    od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height, &geometry);
  if (color_status != OD_COLOR_OK || geometry.layout == OD_COLOR_LAYOUT_SPLIT_HALVES) {
    od_log_info("dw start err unsupported color geometry cs=%u status=%d layout=%d",
                (unsigned)d->color_scheme, (int)color_status,
                color_status == OD_COLOR_OK ? (int)geometry.layout : -1);
    return -2;
  }

  int panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    od_log_info("dw start err bad panel_ic_type=%u", (unsigned)d->panel_ic_type);
    return -2;
  }

  opendisplay_display_abort();
  dw_init_mark("after abort");
  if (!display_power_set(true)) {
    od_log_info("dw start err panel power unavailable");
    return -3;
  }
  memset(&s_epd, 0, sizeof(s_epd));
  if (bbepSetPanelType(&s_epd, panel) != BBEP_SUCCESS) {
    od_log_info("dw start err setPanelType panel=%d", panel);
    display_power_set(false);
    return -3;
  }
  if ((s_epd.iFlags & BBEP_SPLIT_BUFFER) != 0u) {
    od_log_info("dw start err split-panel transport is not hardware-qualified");
    display_power_set(false);
    return -4;
  }
  dw_init_mark("after setPanelType");

  bbepSetRotation(&s_epd, (int)d->rotation * 90);
  bbepInitIO(&s_epd, d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin, 0);
  dw_init_mark("after initIO");
  od_bbep_wake(&s_epd);
  dw_init_mark("after wake (reset + busy)");
  od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
  od_bbep_send_panel_init_full(&s_epd);
  dw_init_mark("after pInitFull");
  bbepSetAddrWindow(&s_epd, 0, 0, d->pixel_width, d->pixel_height);
  dw_init_mark("after setAddrWindow");

  s_color_scheme = d->color_scheme;
  s_plane_size = geometry.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
      ? geometry.part_bytes[0] : 0u;
  s_plane2_started = false;
  s_total_bytes = geometry.total_bytes;
  bbepStartWrite(&s_epd, geometry.initial_plane == OD_COLOR_PLANE_0 ? PLANE_0 : PLANE_1);
  dw_init_mark("after startWrite");

  s_written_bytes = 0;
  s_dw_chunk_n = 0;
  s_dw_log_pct = 0;
  s_dw_trailing_ignores = 0;
  s_dw_compressed = (payload != nullptr && payload_len >= 4u);
  s_dw_decompressed_total = 0;
  s_active = true;

  if (s_dw_compressed) {
    if ((d->transmission_modes & TRANSMISSION_MODE_STREAMING_DECOMPRESSION) == 0u) {
      od_log_info("dw start err streaming_decompression not enabled in transmission_modes=0x%02X",
             (unsigned)d->transmission_modes);
      opendisplay_display_abort();
      return -4;
    }
    s_dw_decompressed_total =
      (uint32_t)payload[0]
      | ((uint32_t)payload[1] << 8)
      | ((uint32_t)payload[2] << 16)
      | ((uint32_t)payload[3] << 24);
    if (s_dw_decompressed_total != s_total_bytes) {
      od_log_info("dw start err zlib size %lu != %lu",
             (unsigned long)s_dw_decompressed_total, (unsigned long)s_total_bytes);
      opendisplay_display_abort();
      return -5;
    }
    od_zlib_pump_reset(s_dw_decompressed_total);
    if (payload_len > 4u) {
      if (!zlib_stream_to_direct_write(payload + 4, (uint32_t)payload_len - 4u, false)) {
        opendisplay_display_abort();
        return -6;
      }
    }
  } else if (payload_len != 0u) {
    od_log_info("dw start note non-empty payload len=%u (ignored)", (unsigned)payload_len);
  }

  od_log_info("dw start total=%lu B bpp=%u cs=%u panel=%u %ux%u layout=%d%s",
         (unsigned long)s_total_bytes, (unsigned)geometry.bits_per_pixel,
         (unsigned)s_color_scheme, (unsigned)d->panel_ic_type, (unsigned)d->pixel_width,
         (unsigned)d->pixel_height, (int)geometry.layout,
         s_dw_compressed ? " zlib" : "");
  return 0;
}

extern "C" int opendisplay_display_direct_write_data(const uint8_t *payload, uint16_t payload_len)
{
  if (s_partial.active) {
    if (payload == nullptr || payload_len == 0u) {
      return 0;
    }
    if (!partial_consume_bytes((uint8_t *)(void *)payload, payload_len)) {
      s_displayed_etag = 0;
      partial_cleanup();
      return -4;
    }
    return 0;
  }

  if (!s_active || payload == nullptr || payload_len == 0u) {
    od_log_info("dw data bad arg active=%d len=%u", (int)s_active, (unsigned)payload_len);
    return -1;
  }

  if (s_dw_compressed) {
    const uint32_t written_before = s_written_bytes;
    if (!zlib_stream_to_direct_write(payload, payload_len, false)) {
      opendisplay_display_abort();
      return -3;
    }
    if (s_written_bytes > written_before) {
      s_dw_chunk_n++;
    }
    return 0;
  }

  uint32_t remaining = (s_written_bytes < s_total_bytes) ? (s_total_bytes - s_written_bytes) : 0u;
  if (remaining == 0u) {
    if (payload_len > 0u) {
      if (s_dw_trailing_ignores < 4u) {
        od_log_info("dw data ignore trailing chunk #%u len=%u (have %lu/%lu B)",
               (unsigned)s_dw_trailing_ignores + 1u, (unsigned)payload_len,
               (unsigned long)s_written_bytes, (unsigned long)s_total_bytes);
        s_dw_trailing_ignores++;
      }
    }
    return 0;
  }

  const uint32_t written_before = s_written_bytes;
  if (dw_stream_raw_bytes(payload, payload_len) != 0) {
    opendisplay_display_abort();
    return -2;
  }
  if (s_written_bytes > written_before) {
    s_dw_chunk_n++;
  }
  return 0;
}

/* Stage 1: stream finalize/validation only, so the caller can ack 0x72 before
 * the blocking refresh (matches the nRF52840 Firmware response ordering). */
extern "C" int opendisplay_display_direct_write_end_prepare(const uint8_t *payload, uint16_t payload_len)
{
  if (s_partial.active) {
    if (payload != nullptr && payload_len > 1u) {
      s_displayed_etag = 0;
      partial_cleanup();
      return -4;
    }
    if (s_partial.compressed) {
      if (s_partial.bytes_received == 0u || !zlib_stream_to_partial_write(nullptr, 0, true)) {
        s_displayed_etag = 0;
        partial_cleanup();
        return -4;
      }
    } else if (s_partial.bytes_written != s_partial.expected_stream_size) {
      s_displayed_etag = 0;
      partial_cleanup();
      return -4;
    }
    return 0;
  }

  if (!s_active) {
    od_log_info("dw end err inactive");
    return -1;
  }
  if (s_dw_compressed) {
    if (!zlib_stream_to_direct_write(nullptr, 0, true)) {
      od_log_info("dw end err zlib finalize");
      opendisplay_display_abort();
      return -3;
    }
  }
  if (s_written_bytes < s_total_bytes) {
    od_log_info("dw end err incomplete wr=%lu need=%lu", (unsigned long)s_written_bytes,
         (unsigned long)s_total_bytes);
    opendisplay_display_abort();
    return -2;
  }
  return 0;
}

/* Stage 2: panel refresh; call only after a successful _prepare(). */
extern "C" int opendisplay_display_direct_write_end_refresh(const uint8_t *payload, uint16_t payload_len, bool *refresh_ok)
{
  if (s_partial.active) {
    int refresh_mode = REFRESH_PARTIAL;
    if (payload != nullptr && payload_len >= 1u) {
      if (payload[0] == REFRESH_FULL) {
        refresh_mode = REFRESH_FULL;
      } else if (payload[0] == REFRESH_FAST) {
        refresh_mode = REFRESH_FAST;
      }
    }

    bool ok = partial_write_to_panel(refresh_mode);
    if (ok) {
      s_displayed_etag = s_partial.new_etag;
    } else {
      s_displayed_etag = 0;
    }
    if (refresh_ok != nullptr) {
      *refresh_ok = ok;
    }
    partial_cleanup();
    /* An EPD refresh can perturb a GT911 sharing the panel power rail; re-probe
     * it (light probe first, full reset fallback) as the Arduino reference does. */
    opendisplay_touch_resume_after_refresh();
    return 0;
  }

  if (!s_active) {
    od_log_info("dw end err inactive");
    return -1;
  }
  if (refresh_ok != nullptr) {
    *refresh_ok = false;
  }

  int refresh_mode = REFRESH_FULL;
  if (payload != nullptr && payload_len >= 1u && payload[0] == 1u) {
    refresh_mode = REFRESH_FAST;
  }

  od_log_info("dw refresh start mode=%d", refresh_mode);
  (void)bbepRefresh(&s_epd, refresh_mode);
  bool ok = wait_for_refresh(60000u);
  od_log_info("dw refresh done ok=%d busy=%d", (int)ok, (int)bbepIsBusy(&s_epd));
  bbepSleep(&s_epd, DEEP_SLEEP);
  s_active = false;
  s_dw_compressed = false;
  s_dw_decompressed_total = 0;
  display_power_set(false);

  {
    uint32_t new_etag = 0;
    bool has_etag = (payload != nullptr && payload_len >= 5u);
    if (has_etag) {
      new_etag = parse_be_u32(payload + 1);
    }
    if (ok && has_etag && new_etag != 0u) {
      s_displayed_etag = new_etag;
    } else if (has_etag) {
      s_displayed_etag = 0;
    }
  }

  if (refresh_ok != nullptr) {
    *refresh_ok = ok;
  }
  /* See note above: re-probe touch after the full-refresh path too. */
  opendisplay_touch_resume_after_refresh();
  return 0;
}

extern "C" int opendisplay_display_pipe_full_start(bool compressed, uint32_t total_size)
{
  uint8_t size_le[4];

  size_le[0] = (uint8_t)(total_size & 0xFFu);
  size_le[1] = (uint8_t)((total_size >> 8) & 0xFFu);
  size_le[2] = (uint8_t)((total_size >> 16) & 0xFFu);
  size_le[3] = (uint8_t)((total_size >> 24) & 0xFFu);
  if (compressed) {
    return opendisplay_display_direct_write_start(size_le, 4u);
  }
  return opendisplay_display_direct_write_start(nullptr, 0u);
}

extern "C" int opendisplay_display_pipe_partial_arm(uint8_t flags, uint32_t old_etag, uint16_t x,
                                                    uint16_t y, uint16_t w, uint16_t h,
                                                    uint32_t total_size, uint8_t *err_code_out)
{
  const struct DisplayConfig *d = display_cfg();
  uint32_t plane_bytes;
  od_color_geometry_t display_geometry;

  if (err_code_out != nullptr) {
    *err_code_out = OD_ERR_PIPE_START_BAD_HEADER;
  }
  if (s_active) {
    opendisplay_display_abort();
  }
  if (s_partial.active) {
    partial_cleanup();
  }
  if (d == nullptr) {
    return -1;
  }
  if ((flags & ~PARTIAL_ALLOWED_FLAGS) != 0u) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PIPE_START_UNKNOWN_FLAG;
    }
    return -1;
  }
  if (d->partial_update_support == 0u
      || od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                                  &display_geometry) != OD_COLOR_OK
      || !display_geometry.partial_supported) {
    s_displayed_etag = 0;
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PIPE_START_PARTIAL_UNSUPPORTED;
    }
    return -1;
  }
  if (old_etag == 0u || old_etag != s_displayed_etag) {
    s_displayed_etag = 0;
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PIPE_START_ETAG_MISMATCH;
    }
    return -1;
  }
  if (w == 0u || h == 0u || (uint32_t)x + w > d->pixel_width || (uint32_t)y + h > d->pixel_height
      || (x & 7u) != 0u || (w & 7u) != 0u) {
    s_displayed_etag = 0;
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PIPE_START_RECT_INVALID;
    }
    return -1;
  }
  plane_bytes = mono_plane_bytes(w, h);
  if (plane_bytes == 0u || total_size != plane_bytes * 2u) {
    s_displayed_etag = 0;
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PIPE_START_SIZE_MISMATCH;
    }
    return -1;
  }
  if ((flags & PARTIAL_FLAG_COMPRESSED) != 0u
      && (d->transmission_modes & TRANSMISSION_MODE_STREAMING_DECOMPRESSION) == 0u) {
    s_displayed_etag = 0;
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PIPE_START_PARTIAL_UNSUPPORTED;
    }
    return -1;
  }

  memset(&s_partial, 0, sizeof(s_partial));
  s_partial.active = true;
  s_partial.compressed = (flags & PARTIAL_FLAG_COMPRESSED) != 0u;
  s_partial.flags = flags;
  s_partial.new_etag = 0;
  s_partial.x = x;
  s_partial.y = y;
  s_partial.width = w;
  s_partial.height = h;
  s_partial.expected_stream_size = total_size;
  s_partial.plane_size = plane_bytes;
  s_partial.current_plane = 0xFFu;
  return 0;
}

extern "C" int opendisplay_display_pipe_partial_prepare(void)
{
  if (!s_partial.active) {
    return -1;
  }
  if (!partial_prepare_panel_ram()) {
    partial_cleanup();
    return -1;
  }
  if (s_partial.compressed) {
    od_zlib_pump_reset(s_partial.expected_stream_size);
  }
  return 0;
}
