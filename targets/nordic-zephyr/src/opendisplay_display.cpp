#include "opendisplay_display.h"
#include "opendisplay_display_color.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
#include "opendisplay_epd_map.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"
#include "opendisplay_touch.h"
#include "board_nrf54.h"
#include "boot_screen.h"
#include "nrf54_gpio.h"
#include "nrf54_zephyr_compat.h"
#include "bb_epaper.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "uzlib.h"
}

void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);

#define OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE 256u

static BBEPAPER s_epd;
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

#ifndef OD_FALLBACK_DISPLAY_PWR_PIN
#define OD_FALLBACK_DISPLAY_PWR_PIN 0x00u
#endif

static void dw_init_mark(const char *tag)
{
  uint32_t now = od_uptime_get_32();
  printf("[OD] dw init %-26s %lu ms\r\n", tag, (unsigned long)(now - s_dw_init_t0));
}

static const struct DisplayConfig *display_cfg(void)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  if (cfg == nullptr || cfg->display_count == 0u) {
    return nullptr;
  }
  return &cfg->displays[0];
}

static void display_park_signal_pin(uint8_t pin_cfg)
{
  nrf54_gpio_park(pin_cfg);
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

static void display_power_set(bool on)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  uint8_t p;

  if (cfg == nullptr) {
    return;
  }
  if (!on) {
    opendisplay_display_park_pins();
  }
  p = cfg->system_config.pwr_pin;
  if (p == 0xFFu) {
    p = OD_FALLBACK_DISPLAY_PWR_PIN;
  }
  nrf54_gpio_configure_output(p, on);
}

void opendisplay_display_power_off(void)
{
  display_power_set(false);
}

static bool wait_for_refresh(uint32_t timeout_ms)
{
  uint32_t elapsed = 0;
  bool saw_busy = false;
  while (elapsed < timeout_ms) {
    bool busy = s_epd.isBusy();
    if (busy) {
      saw_busy = true;
    } else if (saw_busy) {
      return true;
    }
    od_msleep(50);
    elapsed += 50;
  }
  return saw_busy && !s_epd.isBusy();
}

static uint32_t parse_be_u32(const uint8_t *data)
{
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8)
         | (uint32_t)data[3];
}

static uint32_t calc_controller_plane_bytes(uint16_t width, uint16_t height)
{
  return ((uint32_t)(width + 7u) / 8u) * height;
}

static void partial_cleanup(void)
{
  if (!s_partial.active) {
    return;
  }
  if (s_partial_panel_up) {
    s_epd.sleep(DEEP_SLEEP);
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

  s_epd.writeCmd(SSD1608_SET_RAMYPOS);
  uc[0] = (uint8_t)(ramYStart & 0xff);
  uc[1] = (uint8_t)(ramYStart >> 8);
  uc[2] = (uint8_t)(ramYEnd & 0xff);
  uc[3] = (uint8_t)(ramYEnd >> 8);
  s_epd.writeData(uc, 4);

  s_epd.writeCmd(SSD1608_SET_RAMYCOUNT);
  uc[0] = (uint8_t)(ramYStart & 0xff);
  uc[1] = (uint8_t)(ramYStart >> 8);
  s_epd.writeData(uc, 2);
}

static void partial_set_ep426_ram_y(BBEPDISP *pBBEP, int ty, int cy)
{
  uint8_t uc[4];
  int yLast = ty + cy - 1;

  s_epd.writeCmd(SSD1608_SET_RAMYPOS);
  uc[0] = (uint8_t)ty;
  uc[1] = (uint8_t)(ty >> 8);
  uc[2] = (uint8_t)yLast;
  uc[3] = (uint8_t)(yLast >> 8);
  s_epd.writeData(uc, 4);

  s_epd.writeCmd(SSD1608_SET_RAMYCOUNT);
  uc[0] = (uint8_t)ty;
  uc[1] = (uint8_t)(ty >> 8);
  s_epd.writeData(uc, 2);
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

  s_epd.writeCmd(SSD1608_SET_RAMXPOS);
  uc[0] = (uint8_t)(px0 & 0xff);
  uc[1] = (uint8_t)((px0 >> 8) & 0xff);
  uc[2] = (uint8_t)(px1 & 0xff);
  uc[3] = (uint8_t)(px1 >> 8);
  s_epd.writeData(uc, 4);

  s_epd.writeCmd(SSD1608_SET_RAMXCOUNT);
  uc[0] = (uint8_t)(px0 & 0xff);
  uc[1] = (uint8_t)(px0 >> 8);
  s_epd.writeData(uc, 2);
}

static void partial_set_addr_window(BBEPDISP *pBBEP, int x, int y, int cx, int cy)
{
  if (!panel_skips_bbep_set_addr_window(pBBEP)) {
    s_epd.setAddrWindow(x, y, cx, cy);
    return;
  }

  uint8_t uc[4];
  int ty = y;
  cx = (cx + 7) & 0xfff8;

  if (panel_uses_pixel_ram_x(pBBEP)) {
    partial_set_pixel_ram_x(pBBEP, x, cx);
  } else {
    int tx = x / 8;
    s_epd.writeCmd(SSD1608_SET_RAMXPOS);
    uc[0] = (uint8_t)tx;
    uc[1] = (uint8_t)(tx + ((cx - 1) >> 3));
    s_epd.writeData(uc, 2);
    s_epd.writeCmd2(SSD1608_SET_RAMXCOUNT, (uint8_t)tx);
  }

  if (panel_uses_ep426_x_decrement(pBBEP)) {
    partial_set_ep426_ram_y(pBBEP, ty, cy);
  } else if (panel_uses_ep397_y_decrement(pBBEP)) {
    partial_set_ep397_ram_y(pBBEP, ty, cy);
  } else {
    s_epd.writeCmd(SSD1608_SET_RAMYPOS);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    uc[2] = (uint8_t)(ty + cy - 1);
    uc[3] = (uint8_t)((ty + cy - 1) >> 8);
    s_epd.writeData(uc, 4);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    s_epd.writeCmd(SSD1608_SET_RAMYCOUNT);
    s_epd.writeData(uc, 2);
  }
  s_epd.wait();
}

static bool partial_write_stream_bytes(uint8_t *data, uint32_t len)
{
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
      partial_set_addr_window(&s_epd._bbep, s_partial.x, s_partial.y, s_partial.width, s_partial.height);
      s_epd.startWrite(targetPlane);
      s_partial.current_plane = targetPlane;
    }

    uint32_t planeEnd =
      targetPlane == PLANE_1 ? s_partial.plane_size : s_partial.expected_stream_size;
    uint32_t chunk = planeEnd - s_partial.bytes_written;
    if (chunk > len - offset) {
      chunk = len - offset;
    }
    s_epd.writeData(data + offset, (int)chunk);
    s_partial.bytes_written += chunk;
    offset += chunk;
  }
  return true;
}

static bool zlib_stream_to_partial_write(const uint8_t *data, uint32_t len, bool final)
{
  od_zlib_status_t status = od_zlib_stream_push(data, len, final);
  if (status == OD_ZLIB_STATUS_ERROR) {
    printf("[OD] partial zlib push error: %s\r\n", od_zlib_stream_error());
    return false;
  }

  for (;;) {
    size_t bytes_out = 0;
    status = od_zlib_stream_poll(s_decompression_chunk, OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE, &bytes_out);
    if (bytes_out > 0u && !partial_write_stream_bytes(s_decompression_chunk, (uint32_t)bytes_out)) {
      return false;
    }
    if (status == OD_ZLIB_STATUS_OUTPUT_READY) {
      continue;
    }
    if (status == OD_ZLIB_STATUS_NEEDS_INPUT) {
      return !final;
    }
    if (status == OD_ZLIB_STATUS_DONE) {
      return s_partial.bytes_written == s_partial.expected_stream_size;
    }
    printf("[OD] partial zlib poll error: %s\r\n", od_zlib_stream_error());
    return false;
  }
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
  if (panel_skips_reinit_on_partial_refresh(&s_epd._bbep)) {
    if (panel_uses_ep397_y_decrement(&s_epd._bbep)) {
      static const uint8_t u8cmdz3[4] = { 0xf7, 0xd7, 0xff, 0 };
      s_epd.writeCmd2(SSD1608_DISP_CTRL2, u8cmdz3[refresh_mode]);
    } else {
      static const uint8_t u8cmd[4] = { 0xf7, 0xc7, 0xff, 0xc0 };
      s_epd.writeCmd2(SSD1608_DISP_CTRL2, u8cmd[refresh_mode]);
    }
    s_epd.writeCmd(SSD1608_MASTER_ACTIVATE);
    return wait_for_refresh(60000u);
  }
  (void)s_epd.refresh(refresh_mode, false);
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

  display_power_set(true);
  s_epd = BBEPAPER();
  if (s_epd.setPanelType(panel) != BBEP_SUCCESS) {
    display_power_set(false);
    return false;
  }
  s_epd.setRotation((int)d->rotation * 90);
  s_epd.initIO(d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin, 0);
  s_epd.wake();
  {
    const uint8_t *init_seq = s_epd._bbep.pInitPart ? s_epd._bbep.pInitPart : s_epd._bbep.pInitFull;
    bbepSendCMDSequence(&s_epd._bbep, init_seq);
  }
  s_epd.fillScreen(BBEP_WHITE, PLANE_1);
  s_epd.fillScreen(BBEP_WHITE, PLANE_0);
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
  s_epd.sleep(DEEP_SLEEP);
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

  if (d == nullptr) {
    return 0;
  }
  return opendisplay_color_direct_write_total_bytes(d->pixel_width, d->pixel_height, d->color_scheme);
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
  return calc_controller_plane_bytes(width, height);
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
      *err_code_out = OD_ERR_ETAG_MISMATCH;
    }
    return -1;
  }
  if (opendisplay_color_bits_per_pixel(d->color_scheme) != 1) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_PARTIAL_UNSUPPORTED;
    }
    return -1;
  }
  if (rect_w == 0u || rect_h == 0u || (uint32_t)rect_x + rect_w > d->pixel_width
      || (uint32_t)rect_y + rect_h > d->pixel_height) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_RECT_OOB;
    }
    return -1;
  }
  if ((rect_x & 7u) != 0u || (rect_w & 7u) != 0u) {
    if (err_code_out != nullptr) {
      *err_code_out = OD_ERR_RECT_ALIGN;
    }
    return -1;
  }

  uint32_t plane_bytes = calc_controller_plane_bytes(rect_w, rect_h);
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
    od_zlib_stream_reset(expected_size);
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

  printf("[OD] partial start etag=%08lX->%08lX rect=%u,%u %ux%u %s\r\n",
         (unsigned long)old_etag, (unsigned long)new_etag, (unsigned)rect_x, (unsigned)rect_y,
         (unsigned)rect_w, (unsigned)rect_h, s_partial.compressed ? "zlib" : "raw");
  return 0;
}

extern "C" void opendisplay_display_abort(void)
{
  partial_cleanup();
  if (s_active) {
    s_epd.sleep(DEEP_SLEEP);
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
    printf("[OD] dw data #%lu %lu/%lu B (%u%%)%s\r\n", (unsigned long)s_dw_chunk_n,
           (unsigned long)s_written_bytes, (unsigned long)s_total_bytes, (unsigned)pct,
           s_dw_compressed ? " zlib" : "");
    s_dw_log_pct = (pct / 25u) * 25u;
  }
}

static int dw_stream_raw_bytes(const uint8_t *payload, uint32_t payload_len)
{
  uint32_t remaining = (s_written_bytes < s_total_bytes) ? (s_total_bytes - s_written_bytes) : 0u;
  const bool bitplanes = opendisplay_color_is_bitplanes(s_color_scheme);
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
    s_epd.writeData((uint8_t *)(void *)p, (int)chunk);
    p += chunk;
    left -= chunk;
    s_written_bytes += chunk;
    remaining -= chunk;
    if (bitplanes && !s_plane2_started && s_plane_size > 0u && s_written_bytes >= s_plane_size) {
      s_epd.startWrite(PLANE_1);
      s_plane2_started = true;
    }
  }

  if (s_written_bytes > written_before) {
    dw_log_progress();
  }
  return 0;
}

static bool zlib_stream_to_direct_write(const uint8_t *data, uint32_t len, bool final)
{
  od_zlib_status_t status = od_zlib_stream_push(data, len, final);
  if (status == OD_ZLIB_STATUS_ERROR) {
    printf("[OD] zlib push error: %s\r\n", od_zlib_stream_error());
    return false;
  }

  for (;;) {
    size_t bytes_out = 0;
    status = od_zlib_stream_poll(s_decompression_chunk, OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE, &bytes_out);
    if (bytes_out > 0u) {
      uint32_t before = s_written_bytes;
      if (dw_stream_raw_bytes(s_decompression_chunk, (uint32_t)bytes_out) != 0) {
        return false;
      }
      if (s_written_bytes - before != (uint32_t)bytes_out) {
        return false;
      }
      if (s_written_bytes > s_dw_decompressed_total) {
        return false;
      }
    }
    if (status == OD_ZLIB_STATUS_OUTPUT_READY) {
      continue;
    }
    if (status == OD_ZLIB_STATUS_NEEDS_INPUT) {
      return !final;
    }
    if (status == OD_ZLIB_STATUS_DONE) {
      return s_written_bytes == s_dw_decompressed_total;
    }
    printf("[OD] zlib poll error: %s\r\n", od_zlib_stream_error());
    return false;
  }
}

extern "C" void opendisplay_display_boot_apply(void)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;
  if (s_boot_applied || d == nullptr) {
    return;
  }
  s_boot_applied = true;
  panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    return;
  }
  s_epd = BBEPAPER();
  display_power_set(true);
  if (s_epd.setPanelType(panel) != BBEP_SUCCESS) {
    display_power_set(false);
    return;
  }
  s_epd.setRotation((int)d->rotation * 90);
  s_epd.initIO(d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin, 0);
  s_epd.wake();
  s_epd.sendPanelInitFull();
  if ((d->transmission_modes & TRANSMISSION_MODE_CLEAR_ON_BOOT) == 0u) {
    if (!writeBootScreenWithQr(s_epd)) {
      s_epd.fillScreen(BBEP_WHITE);
    }
    (void)s_epd.refresh(REFRESH_FULL, false);
    (void)wait_for_refresh(60000u);
  }
  s_epd.sleep(DEEP_SLEEP);
  display_power_set(false);
}

extern "C" int opendisplay_display_direct_write_start(const uint8_t *payload, uint16_t payload_len)
{
  s_dw_init_t0 = od_uptime_get_32();
  printf("[OD] dw init begin\r\n");

  if (s_partial.active) {
    partial_cleanup();
  }

  const struct DisplayConfig *d = display_cfg();
  if (d == nullptr) {
    printf("[OD] dw start err no display cfg\r\n");
    return -1;
  }
  dw_init_mark("after cfg");

  int panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    printf("[OD] dw start err bad panel_ic_type=%u\r\n", (unsigned)d->panel_ic_type);
    return -2;
  }

  opendisplay_display_abort();
  dw_init_mark("after abort");
  display_power_set(true);
  s_epd = BBEPAPER();
  if (s_epd.setPanelType(panel) != BBEP_SUCCESS) {
    printf("[OD] dw start err setPanelType panel=%d\r\n", panel);
    display_power_set(false);
    return -3;
  }
  dw_init_mark("after setPanelType");

  s_epd.setRotation((int)d->rotation * 90);
  s_epd.initIO(d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin, 0);
  dw_init_mark("after initIO");
  s_epd.wake();
  dw_init_mark("after wake (reset + busy)");
  s_epd.sendPanelInitFull();
  dw_init_mark("after pInitFull");
  s_epd.setAddrWindow(0, 0, d->pixel_width, d->pixel_height);
  dw_init_mark("after setAddrWindow");

  s_color_scheme = d->color_scheme;
  s_plane_size = 0;
  s_plane2_started = false;
  if (opendisplay_color_is_bitplanes(s_color_scheme)) {
    s_plane_size = opendisplay_color_bitplane_plane_bytes(d->pixel_width, d->pixel_height);
  }
  s_total_bytes =
    opendisplay_color_direct_write_total_bytes(d->pixel_width, d->pixel_height, s_color_scheme);
  {
    int sp = opendisplay_color_start_plane(s_color_scheme);
    s_epd.startWrite(sp == 0 ? PLANE_0 : PLANE_1);
  }
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
      printf("[OD] dw start err streaming_decompression not enabled in transmission_modes=0x%02X\r\n",
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
      printf("[OD] dw start err zlib size %lu != %lu\r\n",
             (unsigned long)s_dw_decompressed_total, (unsigned long)s_total_bytes);
      opendisplay_display_abort();
      return -5;
    }
    od_zlib_stream_reset(s_dw_decompressed_total);
    if (payload_len > 4u) {
      if (!zlib_stream_to_direct_write(payload + 4, (uint32_t)payload_len - 4u, false)) {
        opendisplay_display_abort();
        return -6;
      }
    }
  } else if (payload_len != 0u) {
    printf("[OD] dw start note non-empty payload len=%u (ignored)\r\n", (unsigned)payload_len);
  }

  printf("[OD] dw start total=%lu B bpp=%d cs=%u panel=%u %ux%u bitplanes=%d%s\r\n",
         (unsigned long)s_total_bytes, opendisplay_color_bits_per_pixel(s_color_scheme),
         (unsigned)s_color_scheme, (unsigned)d->panel_ic_type, (unsigned)d->pixel_width,
         (unsigned)d->pixel_height, (int)opendisplay_color_is_bitplanes(s_color_scheme),
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
    printf("[OD] dw data bad arg active=%d len=%u\r\n", (int)s_active, (unsigned)payload_len);
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
        printf("[OD] dw data ignore trailing chunk #%u len=%u (have %lu/%lu B)\r\n",
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
    printf("[OD] dw end err inactive\r\n");
    return -1;
  }
  if (s_dw_compressed) {
    if (!zlib_stream_to_direct_write(nullptr, 0, true)) {
      printf("[OD] dw end err zlib finalize\r\n");
      opendisplay_display_abort();
      return -3;
    }
  }
  if (s_written_bytes < s_total_bytes) {
    printf("[OD] dw end err incomplete wr=%lu need=%lu\r\n", (unsigned long)s_written_bytes,
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
    printf("[OD] dw end err inactive\r\n");
    return -1;
  }
  if (refresh_ok != nullptr) {
    *refresh_ok = false;
  }

  int refresh_mode = REFRESH_FULL;
  if (payload != nullptr && payload_len >= 1u && payload[0] == 1u) {
    refresh_mode = REFRESH_FAST;
  }

  printf("[OD] dw refresh start mode=%d\r\n", refresh_mode);
  (void)s_epd.refresh(refresh_mode, false);
  bool ok = wait_for_refresh(60000u);
  printf("[OD] dw refresh done ok=%d busy=%d\r\n", (int)ok, (int)s_epd.isBusy());
  s_epd.sleep(DEEP_SLEEP);
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
      || opendisplay_color_bits_per_pixel(d->color_scheme) != 1) {
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
  plane_bytes = calc_controller_plane_bytes(w, h);
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
    od_zlib_stream_reset(s_partial.expected_stream_size);
  }
  return 0;
}
